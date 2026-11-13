//! Character width.

use unicode_width::UnicodeWidthChar;

/// Number of columns used by `c`: 0 for control and combining characters, 2
/// for wide characters, 1 otherwise.
pub fn char_width(c: char) -> usize {
    c.width().unwrap_or(0).min(2)
}
