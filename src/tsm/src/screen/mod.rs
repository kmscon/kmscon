//! Virtual terminal screen.
//!
//! A [`Screen`] is a grid of [`Cell`]s with a cursor, scroll margins, tab
//! stops, an alternate buffer, a scroll-back buffer and a selection. It does
//! not parse anything: the [`Vte`](crate::Vte) drives it.

mod render;
mod selection;

use std::collections::VecDeque;

use crate::cell::{Attr, AttrFlags, Cell};
use crate::flags::flags;

flags! {
    /// Screen modes, mostly set by the [`Vte`](crate::Vte).
    pub struct ScreenFlags: u32 {
        /// Insert characters instead of overwriting (IRM).
        const INSERT_MODE = 0x01;
        /// Wrap to the next line when writing past the last column (DECAWM).
        const AUTO_WRAP = 0x02;
        /// Cursor positions are relative to the scroll margins (DECOM).
        const REL_ORIGIN = 0x04;
        /// Whole screen is drawn with inverse colors (DECSCNM).
        const INVERSE = 0x08;
        /// The cursor should not be drawn (DECTCEM).
        const HIDE_CURSOR = 0x10;
        const FIXED_POS = 0x20;
        /// The alternate buffer is active.
        const ALTERNATE = 0x40;
    }
}

/// Cursor shape, as selected by DECSCUSR.
#[derive(Copy, Clone, PartialEq, Eq, Hash, Default, Debug)]
pub enum CursorStyle {
    #[default]
    Default,
    BlinkingBlock,
    SteadyBlock,
    BlinkingUnderline,
    SteadyUnderline,
    BlinkingBar,
    SteadyBar,
    /// A value not defined by DECSCUSR.
    Other(u16),
}

impl CursorStyle {
    pub(crate) fn from_code(code: u16) -> Self {
        match code {
            0 => Self::Default,
            1 => Self::BlinkingBlock,
            2 => Self::SteadyBlock,
            3 => Self::BlinkingUnderline,
            4 => Self::SteadyUnderline,
            5 => Self::BlinkingBar,
            6 => Self::SteadyBar,
            n => Self::Other(n),
        }
    }

    /// The DECSCUSR parameter for this style.
    pub fn code(self) -> u16 {
        match self {
            Self::Default => 0,
            Self::BlinkingBlock => 1,
            Self::SteadyBlock => 2,
            Self::BlinkingUnderline => 3,
            Self::SteadyUnderline => 4,
            Self::BlinkingBar => 5,
            Self::SteadyBar => 6,
            Self::Other(n) => n,
        }
    }
}

#[derive(Clone, Debug)]
struct Line {
    /// At least `width` cells for lines on screen. Lines are never shrunk, so
    /// cells beyond the screen width keep their content.
    cells: Vec<Cell>,
}

#[derive(Default, Debug)]
struct Scrollback {
    lines: VecDeque<Line>,
    max: usize,
    /// Index of the first line shown when scrolled back.
    pos: Option<usize>,
}

/// A line of the scroll-back buffer or of the active screen buffer.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
enum Loc {
    Scrollback(usize),
    Screen(usize),
}

/// A selection position. The line is updated whenever lines move, so the
/// selection follows the text while it scrolls into the scroll-back buffer.
#[derive(Copy, Clone, Default, Debug)]
struct SelPos {
    x: usize,
    /// `None` if the line was dropped.
    line: Option<Loc>,
}

#[derive(Copy, Clone, Default, Debug)]
struct Selection {
    active: bool,
    /// Cell where the selection was started.
    begin: SelPos,
    /// First selected cell in terminal order.
    start: SelPos,
    /// Last selected cell in terminal order.
    end: SelPos,
}

/// The terminal screen: a grid of cells with a cursor, scroll margins, tab
/// stops, an alternate buffer, a scroll-back buffer and a selection.
#[derive(Debug)]
pub struct Screen {
    flags: ScreenFlags,
    /// Attributes of erased and new cells.
    def_attr: Attr,

    width: usize,
    height: usize,
    margin_top: usize,
    margin_bottom: usize,
    /// Both buffers always have the same number of lines, at least `height`.
    main: Vec<Line>,
    alt: Vec<Line>,
    sb: Scrollback,

    /// Always within bounds, except that `cursor_x` may be `width` (or
    /// `width + 1` after a wide character) when a wrap is pending.
    cursor_x: usize,
    cursor_y: usize,
    cursor_style: CursorStyle,
    blink_rate_ms: u32,

    tab_ruler: Vec<bool>,
    sel: Selection,
}

impl Default for Screen {
    fn default() -> Self {
        Self::new()
    }
}

impl Screen {
    /// Default screen size.
    pub const DEFAULT_SIZE: (usize, usize) = (80, 24);

    /// Creates an 80x24 screen.
    pub fn new() -> Self {
        let mut screen = Screen {
            flags: ScreenFlags::empty(),
            def_attr: Attr::WHITE_ON_BLACK,
            width: 0,
            height: 0,
            margin_top: 0,
            margin_bottom: 0,
            main: Vec::new(),
            alt: Vec::new(),
            sb: Scrollback::default(),
            cursor_x: 0,
            cursor_y: 0,
            cursor_style: CursorStyle::Default,
            blink_rate_ms: 500,
            tab_ruler: Vec::new(),
            sel: Selection::default(),
        };
        let (w, h) = Self::DEFAULT_SIZE;
        screen.resize(w, h);
        screen
    }

    // --- internal helpers ---------------------------------------------------

    fn new_line(&self, width: usize) -> Line {
        Line { cells: vec![Cell::blank(self.def_attr); width] }
    }

    fn lines(&self) -> &[Line] {
        if self.flags.contains(ScreenFlags::ALTERNATE) { &self.alt } else { &self.main }
    }

    fn lines_mut(&mut self) -> &mut Vec<Line> {
        if self.flags.contains(ScreenFlags::ALTERNATE) { &mut self.alt } else { &mut self.main }
    }

    /// Resets the first `width` cells of screen line `y`.
    fn clear_line(&mut self, y: usize) {
        let (attr, w) = (self.def_attr, self.width);
        for cell in &mut self.lines_mut()[y].cells[..w] {
            *cell = Cell::blank(attr);
        }
    }

    // --- selection tracking --------------------------------------------------
    //
    // Selection positions refer to lines by location, so every operation that
    // moves lines must update them.

    /// Updates the selection after lines moved: `f` maps the old location of a
    /// line to the new one, or to `None` if the line was dropped.
    fn remap_selection(&mut self, f: impl Fn(Loc) -> Option<Loc>) {
        for pos in [&mut self.sel.begin, &mut self.sel.start, &mut self.sel.end] {
            pos.line = pos.line.and_then(&f);
        }
    }

    /// The screen lines `top..=bottom` were rotated up by `num`.
    fn remap_rotate_up(&mut self, top: usize, bottom: usize, num: usize) {
        let max = bottom + 1 - top;
        self.remap_selection(|loc| match loc {
            Loc::Screen(y) if (top..=bottom).contains(&y) => Some(Loc::Screen(top + (y - top + max - num) % max)),
            loc => Some(loc),
        });
    }

    /// The screen lines `top..=bottom` were rotated down by `num`, with the
    /// `num` lines wrapping to the top reversed.
    fn remap_rotate_down(&mut self, top: usize, bottom: usize, num: usize) {
        let max = bottom + 1 - top;
        self.remap_selection(|loc| match loc {
            Loc::Screen(y) if (top..=bottom).contains(&y) => {
                let o = y - top;
                Some(Loc::Screen(top + if o + num < max { o + num } else { max - 1 - o }))
            }
            loc => Some(loc),
        });
    }

    /// The first scroll-back line was dropped.
    fn remap_sb_pop_front(&mut self) {
        self.remap_selection(|loc| match loc {
            Loc::Scrollback(0) => None,
            Loc::Scrollback(i) => Some(Loc::Scrollback(i - 1)),
            loc => Some(loc),
        });
    }

    /// Moves `line`, coming from screen row `from`, to the scroll-back buffer.
    fn link_to_scrollback(&mut self, line: Line, from: usize) {
        if self.sb.max == 0 {
            self.remap_selection(|loc| (loc != Loc::Screen(from)).then_some(loc));
            return;
        }

        if self.sb.lines.len() >= self.sb.max {
            self.sb.lines.pop_front();
            // the view stays on the (new) first line if it was on the old one
            self.sb.pos = self.sb.pos.map(|p| p.saturating_sub(1));
            self.remap_sb_pop_front();
        }
        self.sb.lines.push_back(line);
        let to = Loc::Scrollback(self.sb.lines.len() - 1);
        self.remap_selection(|loc| Some(if loc == Loc::Screen(from) { to } else { loc }));
    }

    /// Moves `num` lines from the end of the scroll-back buffer to the top of
    /// the screen, replacing the lines there.
    fn remove_from_sb(&mut self, num: usize) {
        if self.sb.max == 0 || self.sb.lines.is_empty() {
            return;
        }

        let mut num = num.min(self.sb.lines.len());
        while num > 0 {
            num -= 1;
            let mut line = self.sb.lines.pop_back().expect("scrollback is not empty");
            if line.cells.len() < self.width {
                line.cells.resize(self.width, Cell::blank(self.def_attr));
            }
            let last = self.sb.lines.len();
            if self.sb.pos == Some(last) {
                self.sb.pos = None;
            }
            self.lines_mut()[num] = line;
            self.remap_selection(|loc| match loc {
                Loc::Screen(y) if y == num => None,
                Loc::Scrollback(i) if i == last => Some(Loc::Screen(num)),
                loc => Some(loc),
            });
        }
    }

    fn scroll_region_up(&mut self, num: usize) {
        let (top, bottom) = (self.margin_top, self.margin_bottom);
        let max = bottom + 1 - top;
        let num = num.min(max);
        if num == 0 {
            return;
        }

        if self.flags.contains(ScreenFlags::ALTERNATE) {
            for y in top..top + num {
                self.clear_line(y);
            }
        } else {
            for y in top..top + num {
                let line = self.new_line(self.width);
                let old = std::mem::replace(&mut self.main[y], line);
                self.link_to_scrollback(old, y);
            }
        }
        self.lines_mut()[top..=bottom].rotate_left(num);
        self.remap_rotate_up(top, bottom, num);
    }

    fn scroll_region_down(&mut self, num: usize) {
        let (top, bottom) = (self.margin_top, self.margin_bottom);
        let max = bottom + 1 - top;
        let num = num.min(max);
        if num == 0 {
            return;
        }

        for y in bottom + 1 - num..=bottom {
            self.clear_line(y);
        }
        let lines = &mut self.lines_mut()[top..=bottom];
        lines.rotate_right(num);
        lines[..num].reverse();
        self.remap_rotate_down(top, bottom, num);
    }

    fn write_cell(&mut self, x: usize, y: usize, ch: char, len: usize, attr: &Attr) {
        let (w, insert) = (self.width, self.flags.contains(ScreenFlags::INSERT_MODE));
        let cells = &mut self.lines_mut()[y].cells;

        if insert && x + len < w {
            cells.copy_within(x..w - len, x + len);
        }

        cells[x] = Cell { ch, width: len as u8, attr: *attr };
        for cell in &mut cells[x + 1..(x + len).min(w)] {
            cell.width = 0;
            cell.ch = '\0';
        }
    }

    fn erase_region(&mut self, x_from: usize, y_from: usize, x_to: usize, y_to: usize, protect: bool) {
        let y_to = y_to.min(self.height - 1);
        let x_to = x_to.min(self.width - 1);
        let (w, attr) = (self.width, self.def_attr);
        let mut x_from = x_from;

        for y in y_from..=y_to {
            let to = if y == y_to { x_to } else { w - 1 };
            let cells = &mut self.lines_mut()[y].cells;
            for cell in cells.iter_mut().take(to + 1).skip(x_from) {
                if !(protect && cell.attr.flags.contains(AttrFlags::PROTECT)) {
                    *cell = Cell::blank(attr);
                }
            }
            x_from = 0;
        }
    }

    fn reset_tabs(&mut self) {
        self.tab_ruler.clear();
        self.tab_ruler.extend((0..self.width).map(|i| i % 8 == 0));
    }

    fn reset_scrollback_position(&mut self) {
        self.sb.pos = None;
    }

    // --- size, flags and attributes -----------------------------------------

    /// Number of columns.
    pub fn width(&self) -> usize {
        self.width
    }

    /// Number of rows.
    pub fn height(&self) -> usize {
        self.height
    }

    /// Resizes the screen. Zero sizes are ignored.
    ///
    /// Shrinking the height pushes lines into the scroll-back buffer (if it is
    /// in use), growing it pulls them back. Margins and tab stops are reset.
    pub fn resize(&mut self, width: usize, height: usize) {
        if width == 0 || height == 0 || (width == self.width && height == self.height) {
            return;
        }

        // make sure all lines are big enough for the new and old size
        if height > self.main.len() {
            let w = width.max(self.width);
            while self.main.len() < height {
                let line = self.new_line(w);
                self.main.push(line);
                let line = self.new_line(w);
                self.alt.push(line);
            }
        }
        if width > self.width {
            let blank = Cell::blank(self.def_attr);
            for line in self.main.iter_mut().chain(self.alt.iter_mut()) {
                if line.cells.len() < width {
                    line.cells.resize(width, blank);
                }
            }
        }

        // xterm destroys margins on resize, so do we
        self.margin_top = 0;
        self.margin_bottom = self.height.saturating_sub(1);

        self.width = width;
        self.reset_tabs();
        if self.cursor_x >= self.width {
            self.cursor_x = self.width - 1;
        }

        if height < self.height {
            let diff = self.height - height;
            if self.sb.lines.is_empty() || self.flags.contains(ScreenFlags::ALTERNATE) {
                // only scroll up if the cursor would go off-screen
                if self.cursor_y >= height {
                    self.scroll_region_up(self.cursor_y - height + 1);
                    self.cursor_y = height - 1;
                }
            } else {
                self.scroll_region_up(diff);
                self.cursor_y = self.cursor_y.saturating_sub(diff);
            }
        } else if height > self.height {
            // pull lines back from the scroll-back buffer
            let diff = (height - self.height).min(self.sb.lines.len());
            if diff > 0 && !self.flags.contains(ScreenFlags::ALTERNATE) {
                self.height = height;
                self.margin_bottom = height - 1;
                self.scroll_region_down(diff);
                self.remove_from_sb(diff);
                self.cursor_y += diff;
            }
        }

        self.height = height;
        self.margin_bottom = height - 1;
        if self.cursor_y >= self.height {
            self.cursor_y = self.height - 1;
        }
    }

    /// Scroll margins as 0-based inclusive rows `(top, bottom)`.
    pub fn margins(&self) -> (usize, usize) {
        (self.margin_top, self.margin_bottom)
    }

    /// Sets the scroll margins from 1-based rows, as DECSTBM does. Invalid
    /// values reset the margins to the full screen.
    pub fn set_margins(&mut self, top: usize, bottom: usize) {
        let top = top.max(1);
        if bottom <= top || bottom > self.height {
            self.margin_top = 0;
            self.margin_bottom = self.height - 1;
        } else {
            self.margin_top = top - 1;
            self.margin_bottom = bottom - 1;
        }
    }

    /// Attributes used for erased and new cells.
    pub fn default_attr(&self) -> Attr {
        self.def_attr
    }

    /// Sets the attributes used for erased and new cells.
    pub fn set_default_attr(&mut self, attr: Attr) {
        self.def_attr = attr;
    }

    /// Resets flags, margins, tab stops, selection and scroll-back position,
    /// and switches back to the main buffer.
    pub fn reset(&mut self) {
        self.flags = ScreenFlags::empty();
        self.margin_top = 0;
        self.margin_bottom = self.height - 1;
        self.reset_tabs();
        self.selection_reset();
        self.reset_scrollback_position();
    }

    pub fn flags(&self) -> ScreenFlags {
        self.flags
    }

    /// Sets flags. Switching to the alternate buffer resets the selection and
    /// the scroll-back position.
    pub fn set_flags(&mut self, flags: ScreenFlags) {
        let old = self.flags;
        self.flags.insert(flags);
        if !old.contains(ScreenFlags::ALTERNATE) && flags.contains(ScreenFlags::ALTERNATE) {
            self.selection_reset();
            self.reset_scrollback_position();
        }
    }

    /// Clears flags. Leaving the alternate buffer resets the selection and the
    /// scroll-back position.
    pub fn reset_flags(&mut self, flags: ScreenFlags) {
        let old = self.flags;
        self.flags.remove(flags);
        if old.contains(ScreenFlags::ALTERNATE) && flags.contains(ScreenFlags::ALTERNATE) {
            self.selection_reset();
            self.reset_scrollback_position();
        }
    }

    // --- cursor -------------------------------------------------------------

    /// Cursor position `(x, y)`. `x` may equal the width (or width + 1)
    /// when a line wrap is pending.
    pub fn cursor(&self) -> (usize, usize) {
        (self.cursor_x, self.cursor_y)
    }

    pub fn cursor_style(&self) -> CursorStyle {
        self.cursor_style
    }

    pub fn set_cursor_style(&mut self, style: CursorStyle) {
        self.cursor_style = style;
    }

    /// Cursor blinking period in milliseconds.
    pub fn blink_rate_ms(&self) -> u32 {
        self.blink_rate_ms
    }

    /// Sets the cursor blinking period, clamped to 50..=5000 ms.
    pub fn set_blink_rate_ms(&mut self, ms: u32) {
        self.blink_rate_ms = ms.clamp(50, 5000);
    }

    // --- content ------------------------------------------------------------

    /// The cell at column `x` of row `y` of the active buffer.
    pub fn cell(&self, x: usize, y: usize) -> Option<&Cell> {
        if x >= self.width || y >= self.height {
            return None;
        }
        Some(&self.lines()[y].cells[x])
    }

    /// The cells of row `y` of the active buffer.
    pub fn row(&self, y: usize) -> Option<&[Cell]> {
        if y >= self.height {
            return None;
        }
        Some(&self.lines()[y].cells[..self.width])
    }

    /// Columns with a tab stop.
    pub fn tab_stops(&self) -> impl Iterator<Item = usize> + '_ {
        self.tab_ruler.iter().enumerate().filter(|(_, t)| **t).map(|(i, _)| i)
    }

    // --- scroll-back buffer -------------------------------------------------

    /// Maximum number of lines kept in the scroll-back buffer (0 disables it).
    pub fn max_scrollback(&self) -> usize {
        self.sb.max
    }

    /// Sets the maximum number of scroll-back lines, dropping the oldest
    /// lines if needed. A maximum of 1 is treated as 2.
    pub fn set_max_scrollback(&mut self, max: usize) {
        let max = if max == 1 { 2 } else { max };
        while self.sb.lines.len() > max {
            self.sb.lines.pop_front();
            self.sb.pos = match self.sb.pos {
                Some(p) if !self.sb.lines.is_empty() => Some(p.saturating_sub(1)),
                _ => None,
            };
            self.remap_sb_pop_front();
        }
        self.sb.max = max;
    }

    /// Drops all scroll-back lines.
    pub fn clear_scrollback(&mut self) {
        self.remap_selection(|loc| match loc {
            Loc::Scrollback(_) => None,
            loc => Some(loc),
        });
        self.sb.lines.clear();
        self.sb.pos = None;
    }

    /// Number of lines in the scroll-back buffer.
    pub fn scrollback_len(&self) -> usize {
        self.sb.lines.len()
    }

    /// Index of the first scroll-back line in view, or
    /// [`scrollback_len`](Self::scrollback_len) when not scrolled back.
    pub fn scrollback_pos(&self) -> usize {
        self.sb.pos.unwrap_or(self.sb.lines.len())
    }

    /// Scrolls the view `num` lines back into the scroll-back buffer.
    pub fn scrollback_up(&mut self, num: usize) {
        if num == 0 || self.sb.lines.is_empty() {
            return;
        }
        for _ in 0..num {
            match self.sb.pos {
                Some(0) => return,
                Some(p) => self.sb.pos = Some(p - 1),
                None => self.sb.pos = Some(self.sb.lines.len() - 1),
            }
        }
    }

    /// Scrolls the view `num` lines forward, towards the live screen.
    pub fn scrollback_down(&mut self, num: usize) {
        if let Some(p) = self.sb.pos {
            let p = p.saturating_add(num);
            self.sb.pos = (p < self.sb.lines.len()).then_some(p);
        }
    }

    /// Scrolls back `num` screen heights.
    pub fn scrollback_page_up(&mut self, num: usize) {
        self.scrollback_up(num.saturating_mul(self.height));
    }

    /// Scrolls forward `num` screen heights.
    pub fn scrollback_page_down(&mut self, num: usize) {
        self.scrollback_down(num.saturating_mul(self.height));
    }

    /// Returns the view to the live screen.
    pub fn scrollback_reset(&mut self) {
        self.sb.pos = None;
    }

    // --- editing (driven by the Vte) ----------------------------------------

    /// Writes a character at the cursor and advances it, wrapping and
    /// scrolling as needed. Zero-width characters are ignored.
    pub(crate) fn write(&mut self, ch: char, attr: &Attr) {
        let len = crate::unicode::char_width(ch);
        if len == 0 {
            return;
        }

        let last = if self.cursor_y <= self.margin_bottom || self.cursor_y >= self.height {
            self.margin_bottom
        } else {
            self.height - 1
        };

        if self.cursor_x >= self.width {
            if self.flags.contains(ScreenFlags::AUTO_WRAP) {
                self.cursor_x = 0;
                self.cursor_y += 1;
            } else {
                self.cursor_x = self.width - 1;
            }
        }

        if self.cursor_y > last {
            self.cursor_y = last;
            self.scroll_region_up(1);
        }

        self.write_cell(self.cursor_x, self.cursor_y, ch, len, attr);
        self.cursor_x += len;
    }

    pub(crate) fn newline(&mut self) {
        self.move_down(1, true);
        self.move_line_home();
    }

    pub(crate) fn scroll_up(&mut self, num: usize) {
        self.scroll_region_up(num);
    }

    pub(crate) fn scroll_down(&mut self, num: usize) {
        self.scroll_region_down(num);
    }

    /// Moves the cursor to `(x, y)`, relative to the top margin in origin
    /// mode.
    pub(crate) fn move_to(&mut self, x: usize, y: usize) {
        let (y, last) = if self.flags.contains(ScreenFlags::REL_ORIGIN) {
            (self.margin_top.saturating_add(y), self.margin_bottom)
        } else {
            (y, self.height - 1)
        };
        self.cursor_x = x.min(self.width - 1);
        self.cursor_y = y.min(last);
    }

    /// Moves the cursor to absolute screen coordinates. In origin mode the
    /// position is kept within the scroll margins.
    pub(crate) fn move_to_abs(&mut self, x: usize, y: usize) {
        let y = if self.flags.contains(ScreenFlags::REL_ORIGIN) { y.saturating_sub(self.margin_top) } else { y };
        self.move_to(x, y);
    }

    pub(crate) fn move_up(&mut self, num: usize, scroll: bool) {
        if num == 0 {
            return;
        }
        let size = if self.cursor_y >= self.margin_top { self.margin_top } else { 0 };
        let diff = self.cursor_y - size;
        if num > diff {
            if scroll {
                self.scroll_region_down(num - diff);
            }
            self.cursor_y = size;
        } else {
            self.cursor_y -= num;
        }
    }

    pub(crate) fn move_down(&mut self, num: usize, scroll: bool) {
        if num == 0 {
            return;
        }
        let size = if self.cursor_y <= self.margin_bottom { self.margin_bottom + 1 } else { self.height };
        let diff = size - self.cursor_y - 1;
        if num > diff {
            if scroll {
                self.scroll_region_up(num - diff);
            }
            self.cursor_y = size - 1;
        } else {
            self.cursor_y += num;
        }
    }

    pub(crate) fn move_left(&mut self, num: usize) {
        if num == 0 {
            return;
        }
        let num = num.min(self.width);
        let x = self.cursor_x.min(self.width - 1);
        self.cursor_x = x.saturating_sub(num);
    }

    pub(crate) fn move_right(&mut self, num: usize) {
        if num == 0 {
            return;
        }
        let num = num.min(self.width);
        self.cursor_x = (self.cursor_x + num).min(self.width - 1);
    }

    pub(crate) fn move_line_home(&mut self) {
        self.cursor_x = 0;
    }

    pub(crate) fn set_tabstop(&mut self) {
        if let Some(t) = self.tab_ruler.get_mut(self.cursor_x) {
            *t = true;
        }
    }

    pub(crate) fn reset_tabstop(&mut self) {
        if let Some(t) = self.tab_ruler.get_mut(self.cursor_x) {
            *t = false;
        }
    }

    pub(crate) fn reset_all_tabstops(&mut self) {
        self.tab_ruler.fill(false);
    }

    pub(crate) fn tab_right(&mut self, num: usize) {
        if num == 0 {
            return;
        }
        let mut x = self.cursor_x;
        for _ in 0..num {
            x = (x + 1..self.width).find(|&j| self.tab_ruler[j]).unwrap_or(self.width.max(x + 1));
            if x + 1 >= self.width {
                break;
            }
        }
        // tabs never cause pending new-lines
        self.cursor_x = x.min(self.width - 1);
    }

    pub(crate) fn tab_left(&mut self, num: usize) {
        if num == 0 {
            return;
        }
        let mut x = self.cursor_x.min(self.width);
        for _ in 0..num {
            match (1..x).rev().find(|&j| self.tab_ruler[j]) {
                Some(j) => x = j,
                None => {
                    x = 0;
                    break;
                }
            }
        }
        self.cursor_x = x;
    }

    pub(crate) fn insert_lines(&mut self, num: usize) {
        if num == 0 || self.cursor_y < self.margin_top || self.cursor_y > self.margin_bottom {
            return;
        }
        let (y, bottom) = (self.cursor_y, self.margin_bottom);
        let max = bottom - y + 1;
        let num = num.min(max);

        for i in 0..num {
            self.clear_line(bottom - i);
        }
        if num < max {
            let lines = &mut self.lines_mut()[y..=bottom];
            lines.rotate_right(num);
            lines[..num].reverse();
            self.remap_rotate_down(y, bottom, num);
        }
        self.cursor_x = 0;
    }

    pub(crate) fn delete_lines(&mut self, num: usize) {
        if num == 0 || self.cursor_y < self.margin_top || self.cursor_y > self.margin_bottom {
            return;
        }
        let (y, bottom) = (self.cursor_y, self.margin_bottom);
        let max = bottom - y + 1;
        let num = num.min(max);

        for i in 0..num {
            self.clear_line(y + i);
        }
        if num < max {
            self.lines_mut()[y..=bottom].rotate_left(num);
            self.remap_rotate_up(y, bottom, num);
        }
        self.cursor_x = 0;
    }

    fn clamp_cursor(&mut self) {
        self.cursor_x = self.cursor_x.min(self.width - 1);
        self.cursor_y = self.cursor_y.min(self.height - 1);
    }

    pub(crate) fn insert_chars(&mut self, num: usize) {
        if num == 0 {
            return;
        }
        self.clamp_cursor();
        let (x, w, attr) = (self.cursor_x, self.width, self.def_attr);
        let num = num.min(w - x);
        let y = self.cursor_y;
        let cells = &mut self.lines_mut()[y].cells;
        cells.copy_within(x..w - num, x + num);
        cells[x..x + num].fill(Cell::blank(attr));
    }

    /// Repeats the character left of the cursor `num` times (REP).
    pub(crate) fn repeat_char(&mut self, num: usize) {
        if num == 0 {
            return;
        }
        self.clamp_cursor();
        let (x, w) = (self.cursor_x, self.width);
        if x == 0 {
            return;
        }
        let num = num.min(w - x);
        let y = self.cursor_y;
        let cells = &mut self.lines_mut()[y].cells;
        let prev = cells[x - 1];
        cells[x..x + num].fill(prev);
        self.cursor_x += num;
    }

    pub(crate) fn delete_chars(&mut self, num: usize) {
        if num == 0 {
            return;
        }
        self.clamp_cursor();
        let (x, w, attr) = (self.cursor_x, self.width, self.def_attr);
        let num = num.min(w - x);
        let y = self.cursor_y;
        let cells = &mut self.lines_mut()[y].cells;
        cells.copy_within(x + num..w, x);
        cells[w - num..w].fill(Cell::blank(attr));
    }

    pub(crate) fn erase_chars(&mut self, num: usize) {
        if num == 0 {
            return;
        }
        let x = self.cursor_x.min(self.width - 1);
        self.erase_region(x, self.cursor_y, x + num - 1, self.cursor_y, false);
    }

    pub(crate) fn erase_cursor_to_end(&mut self, protect: bool) {
        let x = self.cursor_x.min(self.width - 1);
        self.erase_region(x, self.cursor_y, self.width - 1, self.cursor_y, protect);
    }

    pub(crate) fn erase_home_to_cursor(&mut self, protect: bool) {
        self.erase_region(0, self.cursor_y, self.cursor_x, self.cursor_y, protect);
    }

    pub(crate) fn erase_current_line(&mut self, protect: bool) {
        self.erase_region(0, self.cursor_y, self.width - 1, self.cursor_y, protect);
    }

    pub(crate) fn erase_screen_to_cursor(&mut self, protect: bool) {
        self.erase_region(0, 0, self.cursor_x, self.cursor_y, protect);
    }

    pub(crate) fn erase_cursor_to_screen(&mut self, protect: bool) {
        let x = self.cursor_x.min(self.width - 1);
        self.erase_region(x, self.cursor_y, self.width - 1, self.height - 1, protect);
    }

    pub(crate) fn erase_screen(&mut self, protect: bool) {
        self.erase_region(0, 0, self.width - 1, self.height - 1, protect);
    }
}
