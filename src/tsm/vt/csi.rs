//! CSI sequences: cursor movement, editing, SGR, modes and reports.

use super::core::{Core, inter};
use super::mouse::{MouseProtocol, MouseTracking};
use super::{Handler, Leds, VteFlags};
use crate::cell::AttrFlags;
use crate::color::{Color, Rgb, index};
use crate::screen::{CursorStyle, ScreenFlags};

/// Maximum number of CSI parameters, the rest are dropped.
const MAX_ARGS: usize = 16;

/// Parsed CSI parameters. Missing parameters are 0.
struct Args {
    v: [u32; MAX_ARGS],
    len: usize,
}

impl Args {
    fn get(&self, i: usize) -> u32 {
        self.v[i]
    }

    /// Parameter `i`, with 0 meaning 1.
    fn count(&self, i: usize) -> usize {
        self.v[i].max(1) as usize
    }

    fn iter(&self) -> impl Iterator<Item = u32> + '_ {
        self.v[..self.len].iter().copied()
    }
}

impl<H: Handler> Core<H> {
    pub(super) fn csi(&mut self, params: &vte::Params, intermediates: &[u8], action: char) {
        // sub-parameters (':') are not supported
        if params.iter().any(|p| p.len() > 1) {
            return;
        }
        let Some(flags) = inter::parse(intermediates) else {
            return;
        };
        let mut args = Args { v: [0; MAX_ARGS], len: 0 };
        for p in params.iter().take(MAX_ARGS) {
            args.v[args.len] = p[0] as u32;
            args.len += 1;
        }

        let screen = &mut self.screen;
        match action {
            'A' => screen.move_up(args.count(0), false),   // CUU
            'B' => screen.move_down(args.count(0), false), // CUD
            'C' => screen.move_right(args.count(0)),       // CUF
            'D' => screen.move_left(args.count(0)),        // CUB
            'd' => {
                // VPA
                let (x, _) = screen.cursor();
                screen.move_to(x, args.count(0) - 1);
            }
            'E' => {
                // CNL
                screen.move_down(args.count(0), false);
                screen.move_line_home();
            }
            'e' => {
                // VPR
                let (x, y) = self.relative_cursor();
                self.screen.move_to(x, y + args.count(0));
            }
            'F' => {
                // CPL
                screen.move_up(args.count(0), false);
                screen.move_line_home();
            }
            'H' | 'f' => screen.move_to(args.count(1) - 1, args.count(0) - 1), // CUP, HVP
            'G' => {
                // CHA
                let (_, y) = self.relative_cursor();
                self.screen.move_to(args.count(0) - 1, y);
            }
            'J' => {
                let protect = flags & inter::WHAT != 0;
                match args.get(0) {
                    0 => screen.erase_cursor_to_screen(protect),
                    1 => screen.erase_screen_to_cursor(protect),
                    2 => screen.erase_screen(protect),
                    _ => {}
                }
            }
            'K' => {
                let protect = flags & inter::WHAT != 0;
                match args.get(0) {
                    0 => screen.erase_cursor_to_end(protect),
                    1 => screen.erase_home_to_cursor(protect),
                    2 => screen.erase_current_line(protect),
                    _ => {}
                }
            }
            'X' => screen.erase_chars(args.count(0)), // ECH
            'm' => {
                // private SGR-like sequences such as XTMODKEYS are not SGR
                if flags & (inter::GT | inter::WHAT) == 0 {
                    self.sgr(&args);
                }
            }
            'p' => {
                if flags & inter::GT != 0 {
                    // XTSMPOINTER (pointer hiding mode) is not implemented
                } else if flags & inter::BANG != 0 {
                    // DECSTR: soft reset
                    self.reset();
                } else if flags & inter::CASH != 0 {
                    // DECRQM is not implemented
                } else if flags & inter::DQUOTE != 0 {
                    self.compat_mode(&args);
                }
            }
            'h' => self.modes(&args, flags, true),  // SM
            'l' => self.modes(&args, flags, false), // RM
            'r' => screen.set_margins(args.get(0) as usize, args.get(1) as usize), // DECSTBM
            'c' => {
                // DA
                if args.len <= 1 && args.get(0) == 0 {
                    if flags == 0 {
                        self.send_primary_da();
                    } else if flags & inter::GT != 0 {
                        self.write(b"\x1b[>1;1;0c");
                    }
                }
            }
            'L' => screen.insert_lines(args.count(0)), // IL
            'M' => screen.delete_lines(args.count(0)), // DL
            'g' => match args.get(0) {
                // TBC
                0 => screen.reset_tabstop(),
                3 => screen.reset_all_tabstops(),
                _ => {}
            },
            '@' => screen.insert_chars(args.count(0)), // ICH
            'P' => screen.delete_chars(args.count(0)), // DCH
            'Z' => screen.tab_left(args.count(0)),     // CBT
            'I' => screen.tab_right(args.count(0)),    // CHT
            'n' => self.device_status(&args),          // DSR
            'S' => screen.scroll_up(args.count(0)),    // SU
            'T' => screen.scroll_down(args.count(0)),  // SD
            't' => {
                let resp = match args.get(0) {
                    18 => 8,
                    19 => 9,
                    _ => return,
                };
                let reply = format!("\x1b[{resp};{};{}t", screen.height(), screen.width());
                self.write(reply.as_bytes());
            }
            'b' => screen.repeat_char(args.count(0)), // REP
            'q' => {
                if flags & inter::SPACE != 0 {
                    // DECSCUSR
                    screen.set_cursor_style(CursorStyle::from_code(args.get(0).min(u16::MAX as u32) as u16));
                } else if flags & inter::DQUOTE != 0 {
                    // DECSCA
                    match args.get(0) {
                        1 => self.pen.flags.insert(AttrFlags::PROTECT),
                        0 | 2 => self.pen.flags.remove(AttrFlags::PROTECT),
                        _ => {}
                    }
                } else if flags == 0 {
                    // DECLL
                    match args.get(0) {
                        0 => self.leds = Leds::empty(),
                        1 => self.leds.insert(Leds::SCROLL_LOCK),
                        2 => self.leds.insert(Leds::NUM_LOCK),
                        3 => self.leds.insert(Leds::CAPS_LOCK),
                        _ => return,
                    }
                    self.handler.leds(self.leds);
                }
            }
            ']' => {
                // cursor blinking rate, as in the linux console
                if args.get(0) == 16 && args.len >= 2 {
                    screen.set_blink_rate_ms(args.get(1));
                }
            }
            _ => {}
        }
    }

    /// Cursor position relative to the origin (top margin in origin mode).
    fn relative_cursor(&self) -> (usize, usize) {
        let (x, y) = self.screen.cursor();
        if self.screen.flags().contains(ScreenFlags::REL_ORIGIN) {
            (x, y.saturating_sub(self.screen.margins().0))
        } else {
            (x, y)
        }
    }

    fn device_status(&mut self, args: &Args) {
        match args.get(0) {
            5 => self.write(b"\x1b[0n"),
            6 => {
                let (x, y) = self.relative_cursor();
                let reply = format!("\x1b[{};{}R", y + 1, x + 1);
                self.write(reply.as_bytes());
            }
            _ => {}
        }
    }

    /// DECSCL: select compatibility level.
    fn compat_mode(&mut self, args: &Args) {
        self.reset();
        match args.get(0) {
            61 => {
                // VT100: use 7-bit mode to avoid character table problems
                self.flags.insert(VteFlags::SEVEN_BIT_MODE);
                self.g[0] = super::charset::Charset::UnicodeLower;
                self.g[1] = super::charset::Charset::DecSupplementalGraphics;
            }
            62..=64 => {
                if matches!(args.get(1), 1 | 2) {
                    self.flags.insert(VteFlags::USE_C1);
                }
                self.flags.insert(VteFlags::EIGHT_BIT_MODE);
                self.g[0] = super::charset::Charset::UnicodeLower;
                self.g[1] = super::charset::Charset::DecSupplementalGraphics;
            }
            // anything else switches back to UTF-8 mode
            _ => {}
        }
    }

    fn sgr(&mut self, args: &Args) {
        let pen = &mut self.pen;
        let a = &args.v[..args.len];
        let mut i = 0;
        while i < a.len() {
            match a[i] {
                0 => {
                    let protect = pen.flags & AttrFlags::PROTECT;
                    *pen = super::core::Pen::default();
                    pen.flags = protect;
                }
                1 => pen.flags.insert(AttrFlags::BOLD),
                2 => pen.flags.insert(AttrFlags::DIM),
                3 => pen.flags.insert(AttrFlags::ITALIC),
                4 => pen.flags.insert(AttrFlags::UNDERLINE),
                5 => pen.flags.insert(AttrFlags::BLINK),
                7 => pen.flags.insert(AttrFlags::INVERSE),
                22 => pen.flags.remove(AttrFlags::BOLD | AttrFlags::DIM),
                23 => pen.flags.remove(AttrFlags::ITALIC),
                24 => pen.flags.remove(AttrFlags::UNDERLINE),
                25 => pen.flags.remove(AttrFlags::BLINK),
                27 => pen.flags.remove(AttrFlags::INVERSE),
                n @ 30..=37 => pen.fg = Color::Indexed((n - 30) as u8),
                39 => pen.fg = Color::Indexed(index::FOREGROUND),
                n @ 40..=47 => pen.bg = Color::Indexed((n - 40) as u8),
                49 => pen.bg = Color::Indexed(index::BACKGROUND),
                n @ 90..=97 => pen.fg = Color::Indexed((n - 90 + 8) as u8),
                n @ 100..=107 => pen.bg = Color::Indexed((n - 100 + 8) as u8),
                n @ (38 | 48) => {
                    let color = match a.get(i + 1) {
                        // 256 colors
                        Some(5) if i + 2 < a.len() => {
                            let code = a[i + 2];
                            i += 2;
                            if code < 16 {
                                Color::Indexed(code as u8)
                            } else {
                                Color::Rgb(self.palette.lookup256(code))
                            }
                        }
                        // true color
                        Some(2) if i + 4 < a.len() => {
                            let rgb = Rgb::new(a[i + 2] as u8, a[i + 3] as u8, a[i + 4] as u8);
                            i += 4;
                            Color::Rgb(rgb)
                        }
                        _ => {
                            i += 1;
                            continue;
                        }
                    };
                    if n == 38 {
                        pen.fg = color;
                    } else {
                        pen.bg = color;
                    }
                }
                _ => {}
            }
            i += 1;
        }
        self.update_erase_attr();
    }

    fn modes(&mut self, args: &Args, flags: u32, set: bool) {
        for mode in args.iter() {
            if flags & inter::WHAT == 0 {
                match mode {
                    2 => self.flags.set(VteFlags::KEYBOARD_ACTION_MODE, set), // KAM
                    4 => self.set_mode(VteFlags::INSERT_REPLACE_MODE, ScreenFlags::INSERT_MODE, set), // IRM
                    12 => self.flags.set(VteFlags::SEND_RECEIVE_MODE, set),   // SRM
                    20 => self.flags.set(VteFlags::LINE_FEED_NEW_LINE_MODE, set), // LNM
                    _ => {}
                }
                continue;
            }

            let tite = self.flags.contains(VteFlags::TITE_INHIBIT_MODE);
            match mode {
                1 => self.flags.set(VteFlags::CURSOR_KEY_MODE, set), // DECCKM
                5 => self.set_mode(VteFlags::INVERSE_SCREEN_MODE, ScreenFlags::INVERSE, set), // DECSCNM
                6 => self.set_mode(VteFlags::ORIGIN_MODE, ScreenFlags::REL_ORIGIN, set), // DECOM
                7 => self.set_mode(VteFlags::AUTO_WRAP_MODE, ScreenFlags::AUTO_WRAP, set), // DECAWM
                8 => self.flags.set(VteFlags::AUTO_REPEAT_MODE, set), // DECARM
                25 => {
                    // DECTCEM
                    self.flags.set(VteFlags::TEXT_CURSOR_MODE, set);
                    if set {
                        self.screen.reset_flags(ScreenFlags::HIDE_CURSOR);
                    } else {
                        self.screen.set_flags(ScreenFlags::HIDE_CURSOR);
                    }
                }
                42 => self.flags.set(VteFlags::NATIONAL_CHARSET_MODE, set), // DECNRCM
                47 if !tite => self.set_alternate(set),
                1047 if !tite => {
                    if !set {
                        self.screen.erase_screen(false);
                    }
                    self.set_alternate(set);
                }
                1048 if !tite => {
                    if set {
                        self.alt_cursor = self.screen.cursor();
                    } else {
                        self.screen.move_to_abs(self.alt_cursor.0, self.alt_cursor.1);
                    }
                }
                1049 if !tite => {
                    if set {
                        self.alt_cursor = self.screen.cursor();
                        self.set_alternate(true);
                        self.screen.erase_screen(false);
                    } else {
                        self.set_alternate(false);
                        self.screen.move_to_abs(self.alt_cursor.0, self.alt_cursor.1);
                    }
                }
                9 | 1000 => {
                    self.mouse.protocol = match (set, mode) {
                        (false, _) => MouseProtocol::None,
                        (true, 9) => MouseProtocol::X10,
                        _ => MouseProtocol::Vt200,
                    };
                    self.mouse.tracking = MouseTracking::Button;
                    self.handler.mouse_mode(self.mouse.tracking, false);
                }
                1002 | 1003 => {
                    self.mouse.tracking = if matches!(self.mouse.protocol, MouseProtocol::X10 | MouseProtocol::Vt200) {
                        MouseTracking::Button
                    } else if !set {
                        MouseTracking::Disabled
                    } else if mode == 1002 {
                        MouseTracking::Button
                    } else {
                        MouseTracking::Any
                    };
                    if self.mouse.protocol != MouseProtocol::None {
                        let pixels = self.mouse.protocol == MouseProtocol::Pixel;
                        self.handler.mouse_mode(self.mouse.tracking, pixels);
                    }
                }
                1006 | 1016 => {
                    let protocol = if mode == 1006 { MouseProtocol::Sgr } else { MouseProtocol::Pixel };
                    self.mouse.protocol = if set { protocol } else { MouseProtocol::None };
                    if !set || self.mouse.tracking != MouseTracking::Disabled {
                        let pixels = mode == 1016 && set;
                        self.handler.mouse_mode(self.mouse.tracking, pixels);
                    }
                }
                2004 => self.bracketed_paste = set,
                // DECANM, DECCOLM, DECSCLM, cursor blinking, DECPFF, DECPEX
                // and unknown modes are ignored
                _ => {}
            }
        }
    }

    fn set_alternate(&mut self, set: bool) {
        if set {
            self.screen.set_flags(ScreenFlags::ALTERNATE);
        } else {
            self.screen.reset_flags(ScreenFlags::ALTERNATE);
        }
    }
}
