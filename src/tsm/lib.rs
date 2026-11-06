//! TSM: a terminal-emulator state machine.
//!
//! TSM implements the common DEC VT100 to VT520 control sequences and the
//! usual xterm extensions. It parses the output of an application into a
//! virtual [`Screen`] and encodes keyboard and mouse input for the
//! application. It does no rendering, font handling or I/O: a terminal
//! emulator feeds it bytes from the pty, sends back what the [`Handler`]
//! receives, and draws the cells returned by [`Screen::render`].
//!
//! ```
//! use tsm::{Keysym, Modifiers, Vte};
//!
//! let mut vte = Vte::new(());
//! vte.input("\x1b[31mred\x1b[m text".as_bytes());
//! let cells = vte.screen().render();
//! assert_eq!(cells[0].ch, 'r');
//! vte.handle_keyboard(Keysym::UP, Keysym::NO_SYMBOL, Modifiers::empty(), None);
//! ```

mod cell;
mod color;
mod ffi;
mod flags;
pub mod keysym;
mod screen;
mod unicode;
mod vt;

pub use cell::{Attr, AttrFlags, Cell, RenderCell};
pub use color::{Color, Palette, Rgb, index};
pub use keysym::Keysym;
pub use screen::{CursorStyle, Screen, ScreenFlags};
pub use unicode::char_width;
pub use vt::{
    Handler, Leds, Modifiers, MouseButton, MouseEvent, MouseEventKind, MouseModifiers, MouseProtocol, MouseTracking,
    Vte, VteFlags,
};
