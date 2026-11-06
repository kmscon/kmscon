//! Terminal emulation: parse the byte stream from the application and drive
//! the [`Screen`], and encode keyboard and mouse input for the application.

mod charset;
mod core;
mod csi;
mod decode;
mod keyboard;
mod mouse;

use crate::cell::Attr;
use crate::color::Palette;
use crate::flags::flags;
use crate::screen::Screen;

use self::core::Core;
use self::decode::Decoder;

pub use self::mouse::{MouseButton, MouseEvent, MouseEventKind, MouseModifiers, MouseProtocol, MouseTracking};

flags! {
    /// Terminal modes.
    pub struct VteFlags: u32 {
        /// DECCKM: cursor keys send application sequences.
        const CURSOR_KEY_MODE = 0x0000_0001;
        /// DECKPAM: keypad sends application sequences.
        const KEYPAD_APPLICATION_MODE = 0x0000_0002;
        /// LNM: line feed implies carriage return, Return sends CR LF.
        const LINE_FEED_NEW_LINE_MODE = 0x0000_0004;
        /// Input is 8-bit (no UTF-8).
        const EIGHT_BIT_MODE = 0x0000_0008;
        /// Input is 7-bit (high bit stripped).
        const SEVEN_BIT_MODE = 0x0000_0010;
        /// S8C1T: 8-bit C1 controls requested (not used for output).
        const USE_C1 = 0x0000_0020;
        /// KAM (not implemented).
        const KEYBOARD_ACTION_MODE = 0x0000_0040;
        /// IRM: insert mode.
        const INSERT_REPLACE_MODE = 0x0000_0080;
        /// SRM: no local echo. When cleared, input is echoed to the screen.
        const SEND_RECEIVE_MODE = 0x0000_0100;
        /// DECTCEM: cursor visible.
        const TEXT_CURSOR_MODE = 0x0000_0200;
        /// DECSCNM: inverse screen.
        const INVERSE_SCREEN_MODE = 0x0000_0400;
        /// DECOM: origin mode.
        const ORIGIN_MODE = 0x0000_0800;
        /// DECAWM: auto wrap.
        const AUTO_WRAP_MODE = 0x0000_1000;
        /// DECARM (not implemented).
        const AUTO_REPEAT_MODE = 0x0000_2000;
        /// DECNRCM (not implemented).
        const NATIONAL_CHARSET_MODE = 0x0000_4000;
        /// Erased cells get the current background color.
        const BACKGROUND_COLOR_ERASE_MODE = 0x0000_8000;
        /// Internal: prefix the next output with ESC (Alt key).
        const PREPEND_ESCAPE = 0x0001_0000;
        /// Prevent switching to the alternate screen.
        const TITE_INHIBIT_MODE = 0x0002_0000;
    }
}

flags! {
    /// Keyboard modifiers for [`Vte::handle_keyboard`].
    pub struct Modifiers: u32 {
        const SHIFT = 1 << 0;
        const LOCK = 1 << 1;
        const CONTROL = 1 << 2;
        const ALT = 1 << 3;
        const LOGO = 1 << 4;
    }
}

flags! {
    /// Keyboard LEDs, as set by DECLL.
    pub struct Leds: u32 {
        const SCROLL_LOCK = 1 << 0;
        const NUM_LOCK = 1 << 1;
        const CAPS_LOCK = 1 << 2;
    }
}

/// Receives the output and events of a [`Vte`]. All methods default to doing
/// nothing.
pub trait Handler {
    /// Bytes to send to the application (replies, keyboard and mouse input).
    fn write(&mut self, _data: &[u8]) {}

    /// An OSC string not handled internally, without the `ESC ]` introducer
    /// and terminator, truncated to 127 bytes.
    fn osc(&mut self, _data: &[u8]) {}

    /// BEL was received.
    fn bell(&mut self) {}

    /// The keyboard LEDs changed (DECLL).
    fn leds(&mut self, _leds: Leds) {}

    /// The mouse events the application is interested in changed.
    /// `pixels` is set when coordinates must be reported in pixels.
    fn mouse_mode(&mut self, _tracking: MouseTracking, _pixels: bool) {}
}

impl Handler for () {}

/// A terminal emulator state machine.
///
/// It owns the [`Screen`] it draws on and a [`Handler`] that receives the
/// bytes to send back to the application and other events.
///
/// ```
/// use tsm::Vte;
///
/// #[derive(Default)]
/// struct Pty(Vec<u8>);
///
/// impl tsm::Handler for Pty {
///     fn write(&mut self, data: &[u8]) {
///         self.0.extend_from_slice(data);
///     }
/// }
///
/// let mut vte = Vte::new(Pty::default());
/// vte.input(b"hello \x1b[1mworld\x1b[m\x1b[6n");
/// assert_eq!(vte.screen().cell(6, 0).unwrap().ch, 'w');
/// assert_eq!(vte.handler().0, b"\x1b[1;12R");
/// ```
pub struct Vte<H: Handler = ()> {
    parser: vte::Parser,
    decoder: Decoder,
    seq: Seq,
    /// Output of the decoder for one input byte.
    scratch: Vec<u8>,
    core: Core<H>,
}

/// Escape sequence structure of the bytes given to the parser, tracked to
/// work around limitations of `vte`:
/// * OSC strings are collected here (and not passed to `vte`), as `vte` keeps
///   at most 16 `;`-separated parameters and buffers without limit.
/// * Non-ASCII bytes inside a CSI sequence are replaced by `:` so that the
///   sequence is ignored, as `vte` would silently skip them.
#[derive(Copy, Clone, PartialEq, Eq, Default, Debug)]
enum Seq {
    #[default]
    Ground,
    Esc,
    EscIntermediate,
    Csi,
    Osc,
}

impl<H: Handler> Vte<H> {
    /// Creates a terminal with a new 80x24 screen.
    pub fn new(handler: H) -> Self {
        Self::with_screen(Screen::new(), handler)
    }

    /// Creates a terminal drawing on `screen`, which is reset and erased.
    pub fn with_screen(screen: Screen, handler: H) -> Self {
        Vte {
            parser: vte::Parser::new(),
            decoder: Decoder::default(),
            seq: Seq::Ground,
            scratch: Vec::with_capacity(8),
            core: Core::new(screen, handler),
        }
    }

    pub fn screen(&self) -> &Screen {
        &self.core.screen
    }

    pub fn screen_mut(&mut self) -> &mut Screen {
        &mut self.core.screen
    }

    pub fn handler(&self) -> &H {
        &self.core.handler
    }

    pub fn handler_mut(&mut self) -> &mut H {
        &mut self.core.handler
    }

    /// Consumes the terminal, returning its screen and handler.
    pub fn into_parts(self) -> (Screen, H) {
        (self.core.screen, self.core.handler)
    }

    /// Current terminal modes.
    pub fn flags(&self) -> VteFlags {
        self.core.flags
    }

    /// Mouse reporting protocol requested by the application.
    pub fn mouse_protocol(&self) -> MouseProtocol {
        self.core.mouse.protocol
    }

    /// Mouse events requested by the application.
    pub fn mouse_tracking(&self) -> MouseTracking {
        self.core.mouse.tracking
    }

    pub fn palette(&self) -> &Palette {
        &self.core.palette
    }

    /// Changes the color palette. This resets the current attributes and
    /// erases the screen.
    pub fn set_palette(&mut self, palette: Palette) {
        self.core.set_palette(palette);
    }

    /// Default attributes: palette foreground and background, no flags.
    pub fn default_attr(&self) -> Attr {
        self.core.def_attr
    }

    /// Makes the Backspace key send DEL (0x7f) instead of BS (0x08).
    pub fn set_backspace_sends_delete(&mut self, enable: bool) {
        self.core.backspace_sends_delete = enable;
    }

    /// Soft reset: modes, character sets, attributes and saved state go back
    /// to their defaults. The screen content is kept.
    pub fn reset(&mut self) {
        self.parser = vte::Parser::new();
        self.decoder = Decoder::default();
        self.seq = Seq::Ground;
        self.core.reset();
    }

    /// Hard reset: like [`reset`](Self::reset), and also erase the screen and
    /// scroll-back buffer and move the cursor home.
    pub fn hard_reset(&mut self) {
        self.reset();
        self.core.hard_reset();
    }

    /// Parses data received from the application.
    pub fn input(&mut self, data: &[u8]) {
        let mut i = 0;
        while i < data.len() {
            // ASCII is handled the same in all input modes: pass runs of it
            // directly to the parser
            if self.decoder.is_idle() {
                let run = data[i..].iter().position(|&b| b >= 0x80).unwrap_or(data.len() - i);
                if run > 0 {
                    self.advance(&data[i..i + run]);
                    i += run;
                    continue;
                }
            }

            let mut scratch = std::mem::take(&mut self.scratch);
            scratch.clear();
            self.decoder.feed(data[i], self.core.flags, &mut scratch);
            self.advance(&scratch);
            self.scratch = scratch;
            i += 1;
        }
    }

    /// Passes decoded bytes to the parser, see [`Seq`].
    fn advance(&mut self, bytes: &[u8]) {
        // bytes[start..] are still to be passed to the parser
        let mut start = 0;
        for (i, &b) in bytes.iter().enumerate() {
            match self.seq {
                Seq::Osc => {
                    if matches!(b, 0x07 | 0x18 | 0x1a | 0x1b) {
                        // terminator: let the parser dispatch the string
                        self.parser.advance(&mut self.core, &[b]);
                        self.seq = if b == 0x1b { Seq::Esc } else { Seq::Ground };
                    } else if !matches!(b, 0x00..=0x06 | 0x08..=0x17 | 0x19 | 0x1c..=0x1f) {
                        self.core.osc_put(b);
                    }
                    start = i + 1;
                }
                Seq::Csi if b >= 0x80 => {
                    self.parser.advance(&mut self.core, &bytes[start..i]);
                    self.parser.advance(&mut self.core, b":");
                    start = i + 1;
                }
                seq => {
                    self.seq = match (seq, b) {
                        (_, 0x1b) => Seq::Esc,
                        (Seq::Ground, _) => Seq::Ground,
                        (_, 0x18 | 0x1a) => Seq::Ground,
                        // C0 controls are executed within sequences
                        (_, 0x00..=0x1f) => seq,
                        (Seq::Esc, b'[') => Seq::Csi,
                        (Seq::Esc, b']') => Seq::Osc,
                        (Seq::Esc | Seq::EscIntermediate, 0x20..=0x2f) => Seq::EscIntermediate,
                        (Seq::Esc, 0x7f..) => Seq::Esc,
                        (Seq::Csi, 0x40..=0x7e) => Seq::Ground,
                        (Seq::Csi, _) => Seq::Csi,
                        (Seq::EscIntermediate, 0x7f) => Seq::EscIntermediate,
                        _ => Seq::Ground,
                    };
                    if self.seq == Seq::Osc {
                        self.parser.advance(&mut self.core, &bytes[start..=i]);
                        self.core.osc_start();
                        start = i + 1;
                    }
                }
            }
        }
        self.parser.advance(&mut self.core, &bytes[start..]);
    }

    /// Sends input to the application, echoing it locally if SRM is reset.
    fn write_input(&mut self, data: &[u8]) {
        if !self.core.flags.contains(VteFlags::SEND_RECEIVE_MODE) {
            if self.core.flags.contains(VteFlags::PREPEND_ESCAPE) {
                self.input(b"\x1b");
            }
            self.input(data);
        }
        self.core.write(data);
    }

    /// Sends pasted text to the application, enclosed in bracketed paste
    /// markers if the application asked for them.
    pub fn paste(&mut self, data: &[u8]) {
        if self.core.bracketed_paste {
            let mut buf = Vec::with_capacity(data.len() + 12);
            buf.extend_from_slice(b"\x1b[200~");
            buf.extend_from_slice(data);
            buf.extend_from_slice(b"\x1b[201~");
            self.write_input(&buf);
        } else {
            self.write_input(data);
        }
    }
}
