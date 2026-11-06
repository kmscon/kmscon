//! Terminal state and the `vte::Perform` implementation (print, C0 controls,
//! ESC and OSC sequences). CSI sequences live in `csi.rs`.

use super::charset::Charset;
use super::mouse::MouseState;
use super::{Handler, Leds, VteFlags};
use crate::cell::{Attr, AttrFlags};
use crate::color::{Color, Palette, index};
use crate::screen::{CursorStyle, Screen, ScreenFlags};

/// Current graphic rendition, with unresolved palette colors.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub(super) struct Pen {
    pub fg: Color,
    pub bg: Color,
    pub flags: AttrFlags,
}

impl Default for Pen {
    fn default() -> Self {
        Pen { fg: Color::Indexed(index::FOREGROUND), bg: Color::Indexed(index::BACKGROUND), flags: AttrFlags::empty() }
    }
}

impl Pen {
    pub fn resolve(&self, palette: &Palette) -> Attr {
        Attr {
            fg: self.fg.resolve(palette, self.flags.contains(AttrFlags::BOLD)),
            bg: self.bg.resolve(palette, false),
            flags: self.flags,
        }
    }
}

/// State saved by DECSC and restored by DECRC.
#[derive(Copy, Clone, Debug)]
struct SavedState {
    cursor: (usize, usize),
    pen: Pen,
    gl: usize,
    gr: usize,
    cursor_style: CursorStyle,
    wrap_mode: bool,
    origin_mode: bool,
}

impl Default for SavedState {
    fn default() -> Self {
        SavedState {
            cursor: (0, 0),
            pen: Pen::default(),
            gl: 0,
            gr: 1,
            cursor_style: CursorStyle::Default,
            wrap_mode: true,
            origin_mode: false,
        }
    }
}

pub(super) struct Core<H> {
    pub screen: Screen,
    pub handler: H,
    pub palette: Palette,
    /// Palette foreground and background without flags.
    pub def_attr: Attr,
    pub flags: VteFlags,
    pub pen: Pen,
    pub backspace_sends_delete: bool,

    /// G0..G3 and which of them are invoked into GL and GR, plus single
    /// shifts that apply to the next character only.
    pub g: [Charset; 4],
    pub gl: usize,
    pub gr: usize,
    pub glt: Option<usize>,
    pub grt: Option<usize>,

    saved: SavedState,
    pub alt_cursor: (usize, usize),
    pub mouse: MouseState,
    pub bracketed_paste: bool,
    pub leds: Leds,

    /// OSC string being received, truncated to `OSC_MAX_LEN` bytes.
    osc: String,
    /// Partial UTF-8 character of the OSC string.
    osc_utf8: ([u8; 4], usize),
}

/// Escape sequence intermediates (and CSI private markers) understood by tsm.
pub(super) mod inter {
    pub const BANG: u32 = 1 << 0; // !
    pub const CASH: u32 = 1 << 1; // $
    pub const WHAT: u32 = 1 << 2; // ?
    pub const GT: u32 = 1 << 3; // >
    pub const SPACE: u32 = 1 << 4; // ' '
    pub const SQUOTE: u32 = 1 << 5; // '
    pub const DQUOTE: u32 = 1 << 6; // "
    pub const MULT: u32 = 1 << 7; // *
    pub const PLUS: u32 = 1 << 8; // +
    pub const POPEN: u32 = 1 << 9; // (
    pub const PCLOSE: u32 = 1 << 10; // )

    /// Converts intermediate bytes to flags. Sequences with unknown
    /// intermediates are not supported and return `None`.
    pub fn parse(intermediates: &[u8]) -> Option<u32> {
        intermediates.iter().try_fold(0, |acc, b| {
            Some(
                acc | match b {
                    b'!' => BANG,
                    b'$' => CASH,
                    b'?' => WHAT,
                    b'>' => GT,
                    b' ' => SPACE,
                    b'\'' => SQUOTE,
                    b'"' => DQUOTE,
                    b'*' => MULT,
                    b'+' => PLUS,
                    b'(' => POPEN,
                    b')' => PCLOSE,
                    _ => return None,
                },
            )
        })
    }
}

impl<H: Handler> Core<H> {
    pub fn new(screen: Screen, handler: H) -> Self {
        let palette = Palette::default();
        let mut core = Core {
            screen,
            handler,
            palette,
            def_attr: Pen::default().resolve(&palette),
            flags: VteFlags::empty(),
            pen: Pen::default(),
            backspace_sends_delete: false,
            g: [Charset::UnicodeLower; 4],
            gl: 0,
            gr: 1,
            glt: None,
            grt: None,
            saved: SavedState::default(),
            alt_cursor: (0, 0),
            mouse: MouseState::default(),
            bracketed_paste: false,
            leds: Leds::empty(),
            osc: String::new(),
            osc_utf8: ([0; 4], 0),
        };
        core.reset();
        core.screen.erase_screen(false);
        core
    }

    /// Sends data to the application. Never echoed: this is used for replies
    /// generated while parsing.
    pub fn write(&mut self, data: &[u8]) {
        if self.flags.contains(VteFlags::PREPEND_ESCAPE) {
            self.handler.write(b"\x1b");
        }
        self.handler.write(data);
        self.flags.remove(VteFlags::PREPEND_ESCAPE);
    }

    pub fn send_primary_da(&mut self) {
        self.write(b"\x1b[?60;1;6;9;15c");
    }

    /// Attributes for the screen: the resolved pen.
    pub fn attr(&self) -> Attr {
        self.pen.resolve(&self.palette)
    }

    /// With BCE, erased cells take the current pen (except protection).
    pub fn update_erase_attr(&mut self) {
        if self.flags.contains(VteFlags::BACKGROUND_COLOR_ERASE_MODE) {
            let mut attr = self.attr();
            attr.flags.remove(AttrFlags::PROTECT);
            self.screen.set_default_attr(attr);
        }
    }

    pub fn set_palette(&mut self, palette: Palette) {
        self.palette = palette;
        self.def_attr = Pen::default().resolve(&palette);
        self.pen = Pen::default();
        self.screen.set_default_attr(self.def_attr);
        self.screen.erase_screen(false);
    }

    pub fn reset(&mut self) {
        self.flags = VteFlags::TEXT_CURSOR_MODE
            | VteFlags::AUTO_REPEAT_MODE
            | VteFlags::SEND_RECEIVE_MODE
            | VteFlags::AUTO_WRAP_MODE
            | VteFlags::BACKGROUND_COLOR_ERASE_MODE;
        self.screen.reset();
        self.screen.set_flags(ScreenFlags::AUTO_WRAP);

        self.gl = 0;
        self.gr = 1;
        self.glt = None;
        self.grt = None;
        self.g = [Charset::UnicodeLower, Charset::UnicodeUpper, Charset::UnicodeLower, Charset::UnicodeUpper];

        self.mouse = MouseState::default();
        self.bracketed_paste = false;

        self.pen = Pen::default();
        self.screen.set_default_attr(self.def_attr);
        self.saved = SavedState::default();
    }

    pub fn hard_reset(&mut self) {
        self.screen.erase_screen(false);
        self.screen.clear_scrollback();
        self.screen.move_to(0, 0);
    }

    fn save_state(&mut self) {
        self.saved = SavedState {
            cursor: self.screen.cursor(),
            pen: self.pen,
            gl: self.gl,
            gr: self.gr,
            cursor_style: self.screen.cursor_style(),
            wrap_mode: self.flags.contains(VteFlags::AUTO_WRAP_MODE),
            origin_mode: self.flags.contains(VteFlags::ORIGIN_MODE),
        };
    }

    fn restore_state(&mut self) {
        let s = self.saved;
        self.pen = s.pen;
        self.update_erase_attr();
        self.gl = s.gl;
        self.gr = s.gr;
        self.screen.set_cursor_style(s.cursor_style);
        self.set_mode(VteFlags::AUTO_WRAP_MODE, ScreenFlags::AUTO_WRAP, s.wrap_mode);
        self.set_mode(VteFlags::ORIGIN_MODE, ScreenFlags::REL_ORIGIN, s.origin_mode);
        // the saved position is absolute
        self.screen.move_to_abs(s.cursor.0, s.cursor.1);
    }

    /// Sets or resets a mode mirrored in the screen flags.
    pub fn set_mode(&mut self, flag: VteFlags, screen_flag: ScreenFlags, set: bool) {
        self.flags.set(flag, set);
        if set {
            self.screen.set_flags(screen_flag);
        } else {
            self.screen.reset_flags(screen_flag);
        }
    }

    /// Maps a character through the charsets invoked into GL and GR.
    fn map_char(&mut self, c: char) -> char {
        match c as u32 {
            v @ 33..=126 => {
                let g = self.glt.take().unwrap_or(self.gl);
                self.g[g].map(v - 32)
            }
            v @ 161..=254 => {
                let g = self.grt.take().unwrap_or(self.gr);
                self.g[g].map(v - 160)
            }
            _ => c,
        }
    }

    fn write_char(&mut self, c: char) {
        let attr = self.attr();
        self.screen.write(c, &attr);
    }

    fn set_charset(&mut self, flags: u32, set: Charset) -> bool {
        let slot = if flags & inter::POPEN != 0 {
            0
        } else if flags & inter::PCLOSE != 0 {
            1
        } else if flags & inter::MULT != 0 {
            2
        } else if flags & inter::PLUS != 0 {
            3
        } else {
            return false;
        };
        self.g[slot] = set;
        true
    }

    pub fn osc_start(&mut self) {
        self.osc.clear();
        self.osc_utf8.1 = 0;
    }

    /// Adds a byte of the OSC string. The input is valid UTF-8; characters
    /// that do not fit in the buffer are dropped.
    pub fn osc_put(&mut self, byte: u8) {
        let (buf, len) = &mut self.osc_utf8;
        buf[*len] = byte;
        *len += 1;
        let need = match buf[0] {
            0x00..=0x7f => 1,
            0xc0..=0xdf => 2,
            0xe0..=0xef => 3,
            _ => 4,
        };
        if *len < need {
            return;
        }
        let s = std::str::from_utf8(&buf[..*len]).unwrap_or("\u{fffd}");
        if self.osc.len() + s.len() <= OSC_MAX_LEN {
            self.osc.push_str(s);
        }
        *len = 0;
    }

    fn osc_4(&mut self, data: &[u8], end: &[u8]) {
        let mut c = data.iter().copied().peekable();
        while c.peek().is_some() {
            let mut code: u32 = 0;
            while let Some(d @ b'0'..=b'9') = c.peek().copied() {
                code = code.wrapping_mul(10).wrapping_add((d - b'0') as u32);
                c.next();
            }
            if c.next() != Some(b';') {
                break;
            }
            if c.peek() == Some(&b'?') {
                c.next();
                let rgb = self.palette.lookup256(code);
                self.write_xcolor(&format!("4;{code}"), end, rgb);
            } else {
                // setting colors is not supported, skip the value
                while c.next_if(|&b| b != b';').is_some() {}
            }
            if c.next() != Some(b';') {
                break;
            }
        }
    }

    fn write_xcolor(&mut self, code: &str, end: &[u8], rgb: crate::Rgb) {
        let reply = format!("\x1b]{code};rgb:{0:02x}{0:02x}/{1:02x}{1:02x}/{2:02x}{2:02x}", rgb.r, rgb.g, rgb.b);
        let mut reply = reply.into_bytes();
        reply.extend_from_slice(end);
        self.write(&reply);
    }
}

/// Maximum length of an OSC string, longer strings are truncated.
const OSC_MAX_LEN: usize = 127;

impl<H: Handler> vte::Perform for Core<H> {
    fn print(&mut self, c: char) {
        let c = self.map_char(c);
        self.write_char(c);
    }

    fn execute(&mut self, byte: u8) {
        match byte {
            // ENQ: answerback
            0x05 => self.write(b"\x06"),
            0x07 => self.handler.bell(),
            0x08 => self.screen.move_left(1),
            0x09 => self.screen.tab_right(1),
            // LF, VT, FF
            0x0a..=0x0c => {
                if self.flags.contains(VteFlags::LINE_FEED_NEW_LINE_MODE) {
                    self.screen.newline();
                } else {
                    self.screen.move_down(1, true);
                }
            }
            0x0d => self.screen.move_line_home(),
            // SO, SI
            0x0e => self.gl = 1,
            0x0f => self.gl = 0,
            // SUB: show error symbol
            0x1a => self.write_char('\u{bf}'),
            _ => {}
        }
    }

    fn esc_dispatch(&mut self, intermediates: &[u8], _ignore: bool, byte: u8) {
        let Some(flags) = inter::parse(intermediates) else {
            return;
        };

        // character set designation
        let set = match byte {
            b'B' => Some(Charset::UnicodeLower),
            b'<' => Some(Charset::DecSupplementalGraphics),
            b'0' => Some(Charset::DecSpecialGraphics),
            // national charsets are not implemented, use latin-1
            b'A' | b'4' | b'C' | b'5' | b'R' | b'Q' | b'K' | b'Y' | b'E' | b'6' | b'Z' | b'H' | b'7' | b'=' => {
                Some(Charset::UnicodeUpper)
            }
            _ => None,
        };
        if let Some(set) = set
            && self.set_charset(flags, set)
        {
            return;
        }
        if flags & inter::SPACE != 0 {
            match byte {
                b'F' => self.flags.remove(VteFlags::USE_C1), // S7C1T
                b'G' => self.flags.insert(VteFlags::USE_C1), // S8C1T
                _ => {}
            }
            return;
        }
        if flags != 0 {
            return;
        }

        match byte {
            b'D' => self.screen.move_down(1, true),                       // IND
            b'E' => self.screen.newline(),                                // NEL
            b'H' => self.screen.set_tabstop(),                            // HTS
            b'M' => self.screen.move_up(1, true),                         // RI
            b'N' => self.glt = Some(2),                                   // SS2
            b'O' => self.glt = Some(3),                                   // SS3
            b'Z' => self.send_primary_da(),                               // DECID
            b'~' => self.gr = 1,                                          // LS1R
            b'n' => self.gl = 2,                                          // LS2
            b'}' => self.gr = 2,                                          // LS2R
            b'o' => self.gl = 3,                                          // LS3
            b'|' => self.gr = 3,                                          // LS3R
            b'=' => self.flags.insert(VteFlags::KEYPAD_APPLICATION_MODE), // DECKPAM
            b'>' => self.flags.remove(VteFlags::KEYPAD_APPLICATION_MODE), // DECKPNM
            b'c' => {
                // RIS
                self.reset();
                self.hard_reset();
            }
            b'7' => self.save_state(),    // DECSC
            b'8' => self.restore_state(), // DECRC
            _ => {}
        }
    }

    fn csi_dispatch(&mut self, params: &vte::Params, intermediates: &[u8], _ignore: bool, action: char) {
        self.csi(params, intermediates, action);
    }

    fn osc_dispatch(&mut self, _params: &[&[u8]], bell_terminated: bool) {
        // the string was collected by `osc_put`
        let arg = std::mem::take(&mut self.osc);

        // replies use the terminator of the request
        let end: &[u8] = if bell_terminated { b"\x07" } else { b"\x1b\\" };
        if let Some(rest) = arg.strip_prefix("4;") {
            self.osc_4(rest.as_bytes(), end);
        } else if arg.starts_with("10;?") {
            self.write_xcolor("10", end, self.def_attr.fg);
        } else if arg.starts_with("11;?") {
            self.write_xcolor("11", end, self.def_attr.bg);
        } else {
            self.handler.osc(arg.as_bytes());
        }
    }
}
