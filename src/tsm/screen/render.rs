//! Rendering: flatten the visible view into drawable cells.

use super::{Screen, ScreenFlags};
use crate::cell::{AttrFlags, Cell, RenderCell};
use crate::color::Rgb;

fn dim(fg: Rgb, bg: Rgb) -> Rgb {
    Rgb::new(bg.r / 2 + fg.r / 2, bg.g / 2 + fg.g / 2, bg.b / 2 + fg.b / 2)
}

impl Screen {
    /// Renders the visible view (scroll-back position included) into `out`,
    /// row by row, `width * height` cells in total.
    ///
    /// Inverse video, the selection highlight, screen inversion and dim are
    /// resolved into the colors.
    pub fn render_into(&self, out: &mut Vec<RenderCell>) {
        out.clear();
        out.reserve(self.width * self.height);

        let empty = Cell::blank(self.def_attr);
        let screen_inverse = self.flags.contains(ScreenFlags::INVERSE);
        let mut in_sel = self.sel.active && self.selection_starts_above_view();

        for y in 0..self.height {
            let loc = self.view_loc(y);
            let line = self.view_line(y);
            let (sel_start, sel_end) = if self.sel.active {
                (self.sel.start.line == Some(loc), self.sel.end.line == Some(loc))
            } else {
                (false, false)
            };

            for x in 0..self.width {
                let cell = line.cells.get(x).unwrap_or(&empty);
                if sel_start && x == self.sel.start.x {
                    in_sel = !in_sel;
                }

                let attr = &cell.attr;
                let inverse = attr.flags.contains(AttrFlags::INVERSE) ^ in_sel ^ screen_inverse;
                let (mut fg, bg) = if inverse { (attr.bg, attr.fg) } else { (attr.fg, attr.bg) };
                if attr.flags.contains(AttrFlags::DIM) {
                    fg = dim(fg, bg);
                }
                out.push(RenderCell {
                    ch: if cell.ch == '\0' { ' ' } else { cell.ch },
                    fg,
                    bg,
                    flags: attr.flags & (AttrFlags::BOLD | AttrFlags::ITALIC | AttrFlags::UNDERLINE | AttrFlags::BLINK),
                });

                if sel_end && x == self.sel.end.x {
                    in_sel = !in_sel;
                }
            }
        }
    }

    /// Renders the visible view into a new vector, see
    /// [`render_into`](Self::render_into).
    pub fn render(&self) -> Vec<RenderCell> {
        let mut out = Vec::new();
        self.render_into(&mut out);
        out
    }
}
