//! Mouse selection and copy.
//!
//! Selection positions refer to lines of the scroll-back buffer or of the
//! screen, and are updated when lines move (see `remap_selection`), so they
//! follow the text while it scrolls into the scroll-back buffer.

use super::{Line, Loc, Screen, SelPos};

impl Screen {
    /// Returns `loc` if it designates an existing, visible line.
    fn valid(&self, loc: Option<Loc>) -> Option<Loc> {
        loc.filter(|loc| match *loc {
            Loc::Scrollback(i) => i < self.sb.lines.len(),
            Loc::Screen(y) => y < self.height,
        })
    }

    fn line_at(&self, loc: Loc) -> &Line {
        match loc {
            Loc::Scrollback(i) => &self.sb.lines[i],
            Loc::Screen(y) => &self.lines()[y],
        }
    }

    /// The line shown at view row `y`, taking the scroll-back position into
    /// account.
    pub(super) fn view_loc(&self, y: usize) -> Loc {
        let count = self.sb.lines.len();
        match self.sb.pos {
            Some(p) if p + y < count => Loc::Scrollback(p + y),
            Some(p) => Loc::Screen(y - (count - p)),
            None => Loc::Screen(y),
        }
    }

    pub(super) fn view_line(&self, y: usize) -> &Line {
        self.line_at(self.view_loc(y))
    }

    /// Returns `true` if `a` is before `b` in terminal order.
    fn sel_is_before(&self, a: &SelPos, b: &SelPos) -> bool {
        if a.line == b.line {
            return a.x < b.x;
        }
        match (self.valid(a.line), self.valid(b.line)) {
            (Some(Loc::Scrollback(ia)), Some(Loc::Scrollback(ib))) => ia < ib,
            (Some(Loc::Scrollback(_)), _) => true,
            (_, Some(Loc::Scrollback(_))) => false,
            (Some(Loc::Screen(ya)), Some(Loc::Screen(yb))) => ya < yb,
            (_, Some(Loc::Screen(_))) => false,
            _ => true,
        }
    }

    /// Clears the selection.
    pub fn selection_reset(&mut self) {
        self.sel.active = false;
        self.sel.start.line = None;
        self.sel.end.line = None;
    }

    /// Returns `true` if a selection is active.
    pub fn selection_active(&self) -> bool {
        self.sel.active
    }

    /// Starts a selection at view position `(x, y)`.
    pub fn selection_start(&mut self, x: usize, y: usize) {
        if x >= self.width || y >= self.height {
            return;
        }
        self.sel.active = true;
        self.sel.begin = SelPos { x, line: Some(self.view_loc(y)) };
        self.sel.start = self.sel.begin;
        self.sel.end = self.sel.begin;
    }

    /// Extends the selection from its start to view position `(x, y)`.
    pub fn selection_target(&mut self, x: usize, y: usize) {
        if !self.sel.active || x >= self.width || y >= self.height {
            return;
        }
        let target = SelPos { x, line: Some(self.view_loc(y)) };
        if self.sel_is_before(&self.sel.begin, &target) {
            self.sel.start = self.sel.begin;
            self.sel.end = target;
        } else {
            self.sel.start = target;
            self.sel.end = self.sel.begin;
        }
    }

    /// Selects the word at view position `(x, y)`. Words are delimited by
    /// spaces and empty cells. Does nothing on a space.
    pub fn selection_word(&mut self, x: usize, y: usize) {
        if x >= self.width || y >= self.height {
            return;
        }
        let loc = self.view_loc(y);
        self.sel.start = SelPos { x, line: Some(loc) };
        let cells = &self.line_at(loc).cells;
        let ch = |i: usize| cells.get(i).map_or('\0', |c| c.ch);

        if ch(x) == ' ' {
            return;
        }
        let start = (0..=x).rev().find(|&i| ch(i) == ' ').map_or(0, |i| i + 1);
        let mut end = x;
        while end < cells.len() {
            if matches!(ch(end), ' ' | '\n' | '\0') {
                end = end.wrapping_sub(1);
                break;
            }
            end += 1;
        }

        self.sel.start.x = start;
        self.sel.end = SelPos { x: end, line: Some(loc) };
        self.sel.active = true;
    }

    /// Next line in terminal order: through the scroll-back, then the screen.
    fn next_loc(&self, loc: Loc) -> Option<Loc> {
        match loc {
            Loc::Scrollback(i) if i + 1 < self.sb.lines.len() => Some(Loc::Scrollback(i + 1)),
            Loc::Scrollback(_) => Some(Loc::Screen(0)),
            Loc::Screen(y) if y + 1 < self.height => Some(Loc::Screen(y + 1)),
            Loc::Screen(_) => None,
        }
    }

    fn copy_line(&self, loc: Loc, out: &mut String) {
        let cells = &self.line_at(loc).cells;
        let len = cells.iter().rposition(|c| c.ch != '\0').map_or(0, |i| i + 1);
        let start = if self.sel.start.line == Some(loc) { self.sel.start.x } else { 0 };
        let end = if self.sel.end.line == Some(loc) { self.sel.end.x.wrapping_add(1) } else { self.width };

        if start > len {
            return;
        }
        for cell in &cells[start..end.min(len).max(start)] {
            out.push(if cell.ch == '\0' { ' ' } else { cell.ch });
        }
        out.push('\n');
    }

    /// Returns the selected text, or `None` if there is no selection.
    ///
    /// Lines are separated by `'\n'` and empty cells copied as spaces;
    /// trailing empty cells of a line are dropped.
    pub fn selection_copy(&mut self) -> Option<String> {
        if !self.sel.active {
            return None;
        }
        let mut out = String::new();
        if self.sel.start.line.is_none() && self.sel.end.line.is_none() {
            return Some(out);
        }

        // the start line was dropped: copy from the very first line
        if self.sel.start.line.is_none() {
            let first = if self.sb.lines.is_empty() { Loc::Screen(0) } else { Loc::Scrollback(0) };
            self.sel.start = SelPos { x: 0, line: Some(first) };
        }

        let mut loc = self.valid(self.sel.start.line);
        while let Some(l) = loc {
            self.copy_line(l, &mut out);
            if Some(l) == self.sel.end.line {
                break;
            }
            loc = self.next_loc(l);
        }
        out.pop();
        Some(out)
    }

    /// Whether the selection start/end lie in the scroll-back above the view,
    /// used by the renderer to know if the first row starts selected.
    pub(super) fn selection_starts_above_view(&self) -> bool {
        let mut in_sel = self.sel.start.line.is_none() && self.sel.end.line.is_some();
        let above = |pos: &SelPos| match self.valid(pos.line) {
            Some(Loc::Scrollback(i)) => self.sb.pos.is_none_or(|p| i < p),
            _ => false,
        };
        if above(&self.sel.start) {
            in_sel = !in_sel;
        }
        if above(&self.sel.end) {
            in_sel = !in_sel;
        }
        in_sel
    }
}
