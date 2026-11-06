//! Feeds pseudo-random input and API calls to make sure nothing panics.

use tsm::{Keysym, Modifiers, MouseButton, MouseEvent, MouseEventKind, MouseModifiers, Vte};

/// xorshift64*, good enough to generate test input.
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }

    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
}

const FRAGMENTS: &[&[u8]] = &[
    b"\x1b[",
    b"\x1b]",
    b"\x1bP",
    b"\x1b[?",
    b"\x1b(0",
    b"\x1b)<",
    b"\x1bN",
    b"\x1b7",
    b"\x1b8",
    b"\x1bc",
    b";",
    b":",
    b"\x07",
    b"\x1b\\",
    b"\xc2\x9b",
    b"\xc2\x9d",
    b"\xe4\xb8\x96",
    b"\xf0\x9f\x98\x80",
    b"\r\n",
    b"\x1b[?1049h",
    b"\x1b[?1049l",
    b"\x1b[?6h",
    b"\x1b[2;5r",
    b"\x1b[4h",
    b"\x1b[62\"p",
    b"\x1b[61\"p",
    b"\x1b[65\"p",
    b"\x1b[12l",
    b"\x1b[?1006h\x1b[?1003h",
    b"\x1b[?9h",
];

#[test]
fn random_input_does_not_panic() {
    let mut rng = Rng(0x1234_5678_9abc_def1);
    let mut vte = Vte::new(());
    vte.screen_mut().set_max_scrollback(30);

    for _ in 0..20_000 {
        match rng.below(100) {
            0..=59 => {
                let mut buf = Vec::new();
                for _ in 0..rng.below(16) {
                    match rng.below(4) {
                        0 => buf.extend_from_slice(FRAGMENTS[rng.below(FRAGMENTS.len() as u64) as usize]),
                        1 => buf.extend(format!("{}", rng.below(300)).bytes()),
                        2 => buf.push(0x40 + rng.below(0x3f) as u8),
                        _ => buf.push(rng.next() as u8),
                    }
                }
                vte.input(&buf);
            }
            60..=64 => vte.screen_mut().resize(1 + rng.below(40) as usize, 1 + rng.below(20) as usize),
            65..=69 => {
                let s = vte.screen_mut();
                match rng.below(5) {
                    0 => s.scrollback_up(rng.below(40) as usize),
                    1 => s.scrollback_down(rng.below(40) as usize),
                    2 => s.set_max_scrollback(rng.below(40) as usize),
                    3 => s.clear_scrollback(),
                    _ => s.scrollback_reset(),
                }
            }
            70..=79 => {
                let (w, h) = (vte.screen().width() as u64 + 2, vte.screen().height() as u64 + 2);
                let s = vte.screen_mut();
                let (x, y) = (rng.below(w) as usize, rng.below(h) as usize);
                match rng.below(5) {
                    0 => s.selection_start(x, y),
                    1 => s.selection_target(x, y),
                    2 => s.selection_word(x, y),
                    3 => s.selection_reset(),
                    _ => {
                        let _ = s.selection_copy();
                    }
                }
            }
            80..=89 => {
                let keysym = match rng.below(3) {
                    0 => Keysym(0xff00 + rng.below(0x100) as u32),
                    1 => Keysym(0x20 + rng.below(0x5f) as u32),
                    _ => Keysym(rng.next() as u32),
                };
                let mods = Modifiers::from_bits_retain(rng.below(32) as u32);
                vte.handle_keyboard(keysym, Keysym::NO_SYMBOL, mods, char::from_u32(rng.below(0x3000) as u32));
            }
            90..=95 => {
                let ev = MouseEvent {
                    kind: MouseEventKind::from_bits_retain(rng.below(8) as u32),
                    button: MouseButton(rng.below(40) as u8),
                    modifiers: MouseModifiers::from_bits_retain(rng.below(32) as u8),
                    cell_x: rng.next() as u32,
                    cell_y: rng.below(500) as u32,
                    pixel_x: rng.below(5000) as u32,
                    pixel_y: rng.next() as u32,
                };
                vte.handle_mouse(&ev);
            }
            96 => vte.paste(b"pasted\ntext"),
            97 => vte.reset(),
            98 => vte.hard_reset(),
            _ => {
                let _ = vte.screen().render();
            }
        }
    }
}
