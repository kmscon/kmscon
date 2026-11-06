//! Mouse tracking.
//!
//! The application selects a reporting protocol and which events it wants
//! (see <https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-Mouse-Tracking>).
//! The [`Handler::mouse_mode`](super::Handler::mouse_mode) callback tells the
//! terminal which events to pass to [`Vte::handle_mouse`], which encodes them
//! and drops events the application did not ask for.

use super::{Handler, Vte};
use crate::flags::flags;

/// Mouse reporting protocol selected by the application.
#[derive(Copy, Clone, PartialEq, Eq, Hash, Default, Debug)]
pub enum MouseProtocol {
    #[default]
    None,
    /// Mode 9: button presses only, coordinates up to 223.
    X10,
    /// Mode 1000: presses and releases, coordinates up to 223.
    Vt200,
    /// Mode 1006: `CSI < b;x;y M/m`, unlimited coordinates.
    Sgr,
    /// Mode 1016: like SGR but with pixel coordinates.
    Pixel,
}

impl MouseProtocol {
    /// The DEC private mode number of the protocol, 0 for none.
    pub fn code(self) -> u32 {
        match self {
            Self::None => 0,
            Self::X10 => 9,
            Self::Vt200 => 1000,
            Self::Sgr => 1006,
            Self::Pixel => 1016,
        }
    }
}

/// Mouse events the application wants to receive.
#[derive(Copy, Clone, PartialEq, Eq, Hash, Default, Debug)]
pub enum MouseTracking {
    /// No mouse events.
    #[default]
    Disabled,
    /// Button presses and releases, and moves while a button is pressed.
    Button,
    /// All events, including moves without a button pressed.
    Any,
}

impl MouseTracking {
    /// The DEC private mode number (1002 or 1003), 0 if disabled.
    pub fn code(self) -> u32 {
        match self {
            Self::Disabled => 0,
            Self::Button => 1002,
            Self::Any => 1003,
        }
    }
}

/// A mouse button as reported to the application.
#[derive(Copy, Clone, PartialEq, Eq, Hash, Default, Debug)]
pub struct MouseButton(pub u8);

impl MouseButton {
    pub const LEFT: MouseButton = MouseButton(0);
    pub const MIDDLE: MouseButton = MouseButton(1);
    pub const RIGHT: MouseButton = MouseButton(2);
    pub const WHEEL_UP: MouseButton = MouseButton(4);
    pub const WHEEL_DOWN: MouseButton = MouseButton(5);

    /// The button as used in a drag (move with the button pressed).
    pub const fn drag(self) -> MouseButton {
        MouseButton(self.0 + 32)
    }

    fn is_drag(self) -> bool {
        (32..=34).contains(&self.0)
    }
}

flags! {
    /// What happened to the mouse.
    pub struct MouseEventKind: u32 {
        const PRESSED = 1;
        const RELEASED = 2;
        const MOVED = 4;
    }
}

flags! {
    /// Modifier keys held during a mouse event, encoded as in the protocol.
    pub struct MouseModifiers: u8 {
        const SHIFT = 4;
        const META = 8;
        const CTRL = 16;
    }
}

/// A mouse event for [`Vte::handle_mouse`].
#[derive(Copy, Clone, PartialEq, Eq, Hash, Default, Debug)]
pub struct MouseEvent {
    pub kind: MouseEventKind,
    pub button: MouseButton,
    pub modifiers: MouseModifiers,
    /// 0-based cell coordinates.
    pub cell_x: u32,
    pub cell_y: u32,
    /// Pixel coordinates, for [`MouseProtocol::Pixel`].
    pub pixel_x: u32,
    pub pixel_y: u32,
}

#[derive(Copy, Clone, Default, Debug)]
pub(super) struct MouseState {
    pub protocol: MouseProtocol,
    pub tracking: MouseTracking,
    /// Last cell reported for a move, to drop duplicates.
    pub last: (u32, u32),
}

impl<H: Handler> Vte<H> {
    /// Reports a mouse event to the application. Returns `false` if the
    /// application is not interested in it.
    pub fn handle_mouse(&mut self, event: &MouseEvent) -> bool {
        let mouse = self.core.mouse;
        let moved = event.kind.contains(MouseEventKind::MOVED);
        let mut pressed = event.kind.contains(MouseEventKind::PRESSED);
        let (mut x, mut y) = (event.cell_x, event.cell_y);
        let mods = event.modifiers.bits();

        // in button mode only drags are reported, in any mode all moves
        let want_moves = match mouse.protocol {
            MouseProtocol::X10 | MouseProtocol::Vt200 => false,
            _ => match mouse.tracking {
                MouseTracking::Button => event.button.is_drag(),
                MouseTracking::Any => true,
                MouseTracking::Disabled => false,
            },
        };
        if moved && !want_moves {
            return false;
        }

        let button = match event.button {
            MouseButton::WHEEL_UP => 64,
            MouseButton::WHEEL_DOWN => 65,
            MouseButton(b) => b,
        };

        let reply = match mouse.protocol {
            MouseProtocol::None => return false,
            MouseProtocol::X10 | MouseProtocol::Vt200 => {
                if mouse.protocol == MouseProtocol::X10 && event.kind.contains(MouseEventKind::RELEASED) {
                    return false;
                }
                // start at 1,1 in the printable range, saturated to one byte
                let x = x.saturating_add(0x21).min(0xff) as u8;
                let y = y.saturating_add(0x21).min(0xff) as u8;
                // the released button is not encoded
                let button = if event.kind.contains(MouseEventKind::RELEASED) { 3 } else { button };
                let mods = if mouse.protocol == MouseProtocol::X10 { 0 } else { mods };
                vec![0x1b, b'[', b'M', (button | mods).wrapping_add(0x20), x, y]
            }
            MouseProtocol::Sgr => {
                x = x.wrapping_add(1);
                y = y.wrapping_add(1);
                let mut flags = button | mods;
                if moved {
                    if (x, y) == mouse.last {
                        return false;
                    }
                    // hover is reported as button 35, drags as themselves
                    if button < 32 {
                        flags = 35;
                    }
                    pressed = true;
                    self.core.mouse.last = (x, y);
                }
                format!("\x1b[<{flags};{x};{y}{}", if pressed { 'M' } else { 'm' }).into_bytes()
            }
            MouseProtocol::Pixel => {
                let mut flags = button | mods;
                if event.kind == MouseEventKind::MOVED {
                    if button < 32 {
                        flags = 35;
                    }
                    pressed = true;
                }
                let (x, y) = (event.pixel_x, event.pixel_y);
                format!("\x1b[<{flags};{x};{y}{}", if pressed { 'M' } else { 'm' }).into_bytes()
            }
        };
        self.write_input(&reply);
        true
    }
}
