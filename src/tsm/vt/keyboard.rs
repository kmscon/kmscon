//! Keyboard input encoding.

use super::{Handler, Modifiers, Vte, VteFlags};
use crate::keysym::Keysym;

/// xterm modifier parameter for Shift, Alt and Control combinations.
fn csi_modifier(mods: Modifiers) -> Option<u8> {
    let shift = mods.contains(Modifiers::SHIFT);
    let alt = mods.contains(Modifiers::ALT);
    let ctrl = mods.contains(Modifiers::CONTROL);
    let code = 1 + shift as u8 + 2 * alt as u8 + 4 * ctrl as u8;
    (code > 1).then_some(b'0' + code)
}

/// Control character sent for Ctrl + `sym`.
fn ctrl_char(sym: u32) -> Option<u8> {
    let c = u8::try_from(sym).ok()?;
    Some(match c {
        b'2' | b' ' => 0x00,
        b'a'..=b'z' | b'A'..=b'Z' => c & 0x1f,
        b'3' | b'[' | b'{' => 0x1b,
        b'4' | b'\\' | b'|' => 0x1c,
        b'5' | b']' | b'}' => 0x1d,
        b'6' | b'`' | b'~' => 0x1e,
        b'7' | b'/' | b'?' => 0x1f,
        b'8' => 0x7f,
        _ => return None,
    })
}

/// Sequences of F1..F20, without and with Shift.
const FUNCTION_KEYS: [(&[u8], &[u8]); 20] = [
    (b"\x1bOP", b"\x1b[23~"),
    (b"\x1bOQ", b"\x1b[24~"),
    (b"\x1bOR", b"\x1b[25~"),
    (b"\x1bOS", b"\x1b[26~"),
    (b"\x1b[15~", b"\x1b[28~"),
    (b"\x1b[17~", b"\x1b[29~"),
    (b"\x1b[18~", b"\x1b[31~"),
    (b"\x1b[19~", b"\x1b[32~"),
    (b"\x1b[20~", b"\x1b[33~"),
    (b"\x1b[21~", b"\x1b[34~"),
    (b"\x1b[23~", b"\x1b[23;2~"),
    (b"\x1b[24~", b"\x1b[24;2~"),
    (b"\x1b[25~", b"\x1b[25;2~"),
    (b"\x1b[26~", b"\x1b[26;2~"),
    (b"\x1b[28~", b"\x1b[28;2~"),
    (b"\x1b[29~", b"\x1b[29;2~"),
    (b"\x1b[31~", b"\x1b[31;2~"),
    (b"\x1b[32~", b"\x1b[32;2~"),
    (b"\x1b[33~", b"\x1b[33;2~"),
    (b"\x1b[34~", b"\x1b[34;2~"),
];

impl<H: Handler> Vte<H> {
    /// Encodes a key press and sends it to the application.
    ///
    /// * `keysym`: the key pressed.
    /// * `ascii`: the keysym of the same key in an ASCII layout, if the
    ///   active layout is not ASCII ([`Keysym::NO_SYMBOL`] otherwise). It is
    ///   used for Ctrl shortcuts so that Ctrl+C works with any layout.
    /// * `unicode`: the character produced by the key, if any.
    ///
    /// Alt prefixes the sequence with ESC. Returns `false` if the key
    /// produced nothing.
    pub fn handle_keyboard(&mut self, keysym: Keysym, ascii: Keysym, mods: Modifiers, unicode: Option<char>) -> bool {
        if mods.contains(Modifiers::ALT) {
            self.core.flags.insert(VteFlags::PREPEND_ESCAPE);
        }

        if mods.contains(Modifiers::CONTROL) {
            let sym = if ascii == Keysym::NO_SYMBOL { keysym } else { ascii };
            if let Some(c) = ctrl_char(sym.0) {
                self.write_input(&[c]);
                return true;
            }
        }

        if let Some(seq) = self.key_sequence(keysym, mods) {
            self.write_input(&seq);
            return true;
        }

        if let Some(c) = unicode {
            let flags = self.core.flags;
            if flags.contains(VteFlags::SEVEN_BIT_MODE) {
                let b = if c.is_ascii() { c as u8 } else { b'?' };
                self.write_input(&[b]);
            } else if flags.contains(VteFlags::EIGHT_BIT_MODE) {
                let b = u8::try_from(c as u32).unwrap_or(b'?');
                self.write_input(&[b]);
            } else {
                let mut buf = [0; 4];
                self.write_input(c.encode_utf8(&mut buf).as_bytes());
            }
            return true;
        }

        self.core.flags.remove(VteFlags::PREPEND_ESCAPE);
        false
    }

    /// The sequence of a special key, or `None` for other keys.
    fn key_sequence(&mut self, keysym: Keysym, mods: Modifiers) -> Option<Vec<u8>> {
        let flags = self.core.flags;
        let keypad_app = flags.contains(VteFlags::KEYPAD_APPLICATION_MODE);
        let cursor_app = flags.contains(VteFlags::CURSOR_KEY_MODE);
        let shift = mods.contains(Modifiers::SHIFT);
        let ctrl = mods.contains(Modifiers::CONTROL);

        // keypad keys: application sequence or plain character
        let keypad = |app: &[u8], plain: &[u8]| Some(if keypad_app { app } else { plain }.to_vec());
        // Page Up/Down: Alt is handled by the escape prefix
        let page = |n: u8| {
            let m: &[u8] = match (ctrl, shift) {
                (true, true) => b";6",
                (true, false) => b";5",
                (false, true) => b";2",
                (false, false) => b"",
            };
            Some([b"\x1b[", &[n][..], m, b"~"].concat())
        };

        let seq: &[u8] = match keysym {
            Keysym::BACKSPACE => {
                if self.core.backspace_sends_delete {
                    b"\x7f"
                } else {
                    b"\x08"
                }
            }
            Keysym::TAB | Keysym::KP_TAB => b"\t",
            Keysym::ISO_LEFT_TAB => b"\x1b[Z",
            Keysym::LINEFEED => b"\n",
            Keysym::CLEAR => b"\x0b",
            Keysym::SYS_REQ => b"\x15",
            Keysym::ESCAPE => b"\x1b",
            Keysym::KP_ENTER if keypad_app => b"\x1bOM",
            Keysym::KP_ENTER | Keysym::RETURN => {
                if flags.contains(VteFlags::LINE_FEED_NEW_LINE_MODE) {
                    b"\r\n"
                } else {
                    b"\r"
                }
            }
            Keysym::FIND => b"\x1b[1~",
            Keysym::INSERT => b"\x1b[2~",
            Keysym::DELETE => b"\x1b[3~",
            Keysym::SELECT => b"\x1b[4~",
            Keysym::PAGE_UP | Keysym::KP_PAGE_UP => return page(b'5'),
            Keysym::PAGE_DOWN | Keysym::KP_PAGE_DOWN => return page(b'6'),
            Keysym::UP | Keysym::KP_UP => return Some(self.arrow(b'A', mods)),
            Keysym::DOWN | Keysym::KP_DOWN => return Some(self.arrow(b'B', mods)),
            Keysym::RIGHT | Keysym::KP_RIGHT => return Some(self.arrow(b'C', mods)),
            Keysym::LEFT | Keysym::KP_LEFT => return Some(self.arrow(b'D', mods)),
            Keysym::KP_INSERT | Keysym::KP_0 => return keypad(b"\x1bOp", b"0"),
            Keysym(k @ 0xffb1..=0xffb9) => {
                // KP_1 .. KP_9
                let n = (k - Keysym::KP_0.0) as u8;
                return keypad(&[0x1b, b'O', b'p' + n], &[b'0' + n]);
            }
            Keysym::KP_SUBTRACT => return keypad(b"\x1bOm", b"-"),
            Keysym::KP_SEPARATOR => return keypad(b"\x1bOl", b","),
            Keysym::KP_DELETE | Keysym::KP_DECIMAL => return keypad(b"\x1bOn", b"."),
            Keysym::KP_EQUAL | Keysym::KP_DIVIDE => return keypad(b"\x1bOj", b"/"),
            Keysym::KP_MULTIPLY => return keypad(b"\x1bOo", b"*"),
            Keysym::KP_ADD => return keypad(b"\x1bOk", b"+"),
            Keysym::HOME | Keysym::KP_HOME => {
                if cursor_app {
                    b"\x1bOH"
                } else {
                    b"\x1b[H"
                }
            }
            Keysym::END | Keysym::KP_END => {
                if cursor_app {
                    b"\x1bOF"
                } else {
                    b"\x1b[F"
                }
            }
            Keysym::KP_SPACE => b" ",
            Keysym(k @ 0xff91..=0xff94) => {
                // KP_F1 .. KP_F4
                let (plain, shifted) = FUNCTION_KEYS[(k - Keysym::KP_F1.0) as usize];
                if shift { shifted } else { plain }
            }
            Keysym(k @ 0xffbe..=0xffd1) => {
                // F1 .. F20
                let (plain, shifted) = FUNCTION_KEYS[(k - Keysym::F1.0) as usize];
                if shift { shifted } else { plain }
            }
            _ => return None,
        };
        Some(seq.to_vec())
    }

    fn arrow(&mut self, direction: u8, mods: Modifiers) -> Vec<u8> {
        // the modifier is part of the sequence, never prefix with ESC
        self.core.flags.remove(VteFlags::PREPEND_ESCAPE);
        match csi_modifier(mods) {
            Some(m) => vec![0x1b, b'[', b'1', b';', m, direction],
            None if self.core.flags.contains(VteFlags::CURSOR_KEY_MODE) => vec![0x1b, b'O', direction],
            None => vec![0x1b, b'[', direction],
        }
    }
}
