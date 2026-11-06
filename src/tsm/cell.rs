//! Screen cells and their attributes.

use crate::color::Rgb;
use crate::flags::flags;

flags! {
    /// Character attributes.
    pub struct AttrFlags: u8 {
        const BOLD = 1 << 0;
        const ITALIC = 1 << 1;
        const UNDERLINE = 1 << 2;
        /// Swap foreground and background.
        const INVERSE = 1 << 3;
        /// Not erased by selective erase (DECSED/DECSEL).
        const PROTECT = 1 << 4;
        const BLINK = 1 << 5;
        const DIM = 1 << 6;
    }
}

/// Resolved attributes of a cell.
#[derive(Copy, Clone, PartialEq, Eq, Hash, Default, Debug)]
pub struct Attr {
    pub fg: Rgb,
    pub bg: Rgb,
    pub flags: AttrFlags,
}

impl Attr {
    pub(crate) const WHITE_ON_BLACK: Attr =
        Attr { fg: Rgb::new(255, 255, 255), bg: Rgb::new(0, 0, 0), flags: AttrFlags::empty() };
}

/// One cell of the screen.
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub struct Cell {
    /// The character, or `'\0'` if the cell is empty.
    pub ch: char,
    /// Number of columns used by the character. The cell following a wide
    /// (width 2) character has width 0.
    pub width: u8,
    pub attr: Attr,
}

impl Cell {
    pub(crate) fn blank(attr: Attr) -> Self {
        Cell { ch: '\0', width: 1, attr }
    }
}

/// A cell as it should be drawn, see [`Screen::render`](crate::Screen::render).
///
/// Inverse video, selection, screen inversion and dim are already applied to
/// the colors. Only glyph-shaping flags (bold, italic, underline, blink) are
/// kept in `flags`.
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub struct RenderCell {
    /// The character, `' '` for empty cells.
    pub ch: char,
    pub fg: Rgb,
    pub bg: Rgb,
    pub flags: AttrFlags,
}
