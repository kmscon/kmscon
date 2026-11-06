//! Input decoding in front of the escape sequence parser.
//!
//! The `vte` parser only decodes UTF-8 in the ground state and does not know
//! about 7-bit and 8-bit modes or C1 controls encoded as UTF-8. This decoder
//! turns every input byte into a code point according to the current mode and
//! hands the parser either plain UTF-8 or, for C1 controls, the equivalent
//! 7-bit `ESC Fe` sequence.

use super::VteFlags;

#[derive(Default, Debug)]
pub(super) struct Decoder {
    /// Code point being assembled.
    cp: u32,
    /// Continuation bytes still expected.
    need: u8,
    /// Valid range of the next continuation byte.
    lower: u8,
    upper: u8,
}

impl Decoder {
    pub(super) fn is_idle(&self) -> bool {
        self.need == 0
    }

    pub(super) fn feed(&mut self, byte: u8, flags: VteFlags, out: &mut Vec<u8>) {
        let mut sink = |bytes: &[u8]| out.extend_from_slice(bytes);
        if flags.contains(VteFlags::SEVEN_BIT_MODE) {
            sink(&[byte & 0x7f]);
        } else if flags.contains(VteFlags::EIGHT_BIT_MODE) {
            emit(byte as u32, &mut sink);
        } else {
            self.feed_utf8(byte, &mut sink);
        }
    }

    fn feed_utf8(&mut self, byte: u8, sink: &mut impl FnMut(&[u8])) {
        if self.need > 0 {
            if (self.lower..=self.upper).contains(&byte) {
                self.cp = (self.cp << 6) | (byte & 0x3f) as u32;
                self.need -= 1;
                self.lower = 0x80;
                self.upper = 0xbf;
                if self.need == 0 {
                    emit(self.cp, sink);
                }
                return;
            }
            // truncated sequence: replace it and process the byte afresh
            self.need = 0;
            emit(0xfffd, sink);
        }

        (self.lower, self.upper) = (0x80, 0xbf);
        match byte {
            0x00..=0x7f => emit(byte as u32, sink),
            0xc2..=0xdf => {
                self.cp = (byte & 0x1f) as u32;
                self.need = 1;
            }
            0xe0..=0xef => {
                self.cp = (byte & 0x0f) as u32;
                self.need = 2;
                match byte {
                    0xe0 => self.lower = 0xa0,
                    0xed => self.upper = 0x9f,
                    _ => {}
                }
            }
            0xf0..=0xf4 => {
                self.cp = (byte & 0x07) as u32;
                self.need = 3;
                match byte {
                    0xf0 => self.lower = 0x90,
                    0xf4 => self.upper = 0x8f,
                    _ => {}
                }
            }
            _ => emit(0xfffd, sink),
        }
    }
}

/// Passes one code point to the parser.
fn emit(cp: u32, sink: &mut impl FnMut(&[u8])) {
    match cp {
        0x80..=0x9f => sink(&[0x1b, (cp - 0x40) as u8]),
        _ => {
            let c = char::from_u32(cp).unwrap_or('\u{fffd}');
            let mut buf = [0; 4];
            sink(c.encode_utf8(&mut buf).as_bytes());
        }
    }
}
