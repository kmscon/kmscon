//! XKB key symbols understood by [`Vte::handle_keyboard`](crate::Vte::handle_keyboard).
//!
//! Values are identical to the `XKB_KEY_*` constants of xkbcommon. Latin-1
//! keysyms are equal to their code point, so `Keysym(b'a' as u32)` is the `a`
//! key.

/// An XKB keysym.
#[derive(Copy, Clone, PartialEq, Eq, Hash, Default, Debug)]
pub struct Keysym(pub u32);

impl Keysym {
    pub const NO_SYMBOL: Keysym = Keysym(0);
    pub const SPACE: Keysym = Keysym(0x0020);

    pub const BACKSPACE: Keysym = Keysym(0xff08);
    pub const TAB: Keysym = Keysym(0xff09);
    pub const LINEFEED: Keysym = Keysym(0xff0a);
    pub const CLEAR: Keysym = Keysym(0xff0b);
    pub const RETURN: Keysym = Keysym(0xff0d);
    pub const PAUSE: Keysym = Keysym(0xff13);
    pub const SCROLL_LOCK: Keysym = Keysym(0xff14);
    pub const SYS_REQ: Keysym = Keysym(0xff15);
    pub const ESCAPE: Keysym = Keysym(0xff1b);
    pub const DELETE: Keysym = Keysym(0xffff);
    pub const ISO_LEFT_TAB: Keysym = Keysym(0xfe20);

    pub const HOME: Keysym = Keysym(0xff50);
    pub const LEFT: Keysym = Keysym(0xff51);
    pub const UP: Keysym = Keysym(0xff52);
    pub const RIGHT: Keysym = Keysym(0xff53);
    pub const DOWN: Keysym = Keysym(0xff54);
    pub const PAGE_UP: Keysym = Keysym(0xff55);
    pub const PAGE_DOWN: Keysym = Keysym(0xff56);
    pub const END: Keysym = Keysym(0xff57);
    pub const SELECT: Keysym = Keysym(0xff60);
    pub const INSERT: Keysym = Keysym(0xff63);
    pub const FIND: Keysym = Keysym(0xff68);

    pub const KP_SPACE: Keysym = Keysym(0xff80);
    pub const KP_TAB: Keysym = Keysym(0xff89);
    pub const KP_ENTER: Keysym = Keysym(0xff8d);
    pub const KP_F1: Keysym = Keysym(0xff91);
    pub const KP_F2: Keysym = Keysym(0xff92);
    pub const KP_F3: Keysym = Keysym(0xff93);
    pub const KP_F4: Keysym = Keysym(0xff94);
    pub const KP_HOME: Keysym = Keysym(0xff95);
    pub const KP_LEFT: Keysym = Keysym(0xff96);
    pub const KP_UP: Keysym = Keysym(0xff97);
    pub const KP_RIGHT: Keysym = Keysym(0xff98);
    pub const KP_DOWN: Keysym = Keysym(0xff99);
    pub const KP_PAGE_UP: Keysym = Keysym(0xff9a);
    pub const KP_PAGE_DOWN: Keysym = Keysym(0xff9b);
    pub const KP_END: Keysym = Keysym(0xff9c);
    pub const KP_INSERT: Keysym = Keysym(0xff9e);
    pub const KP_DELETE: Keysym = Keysym(0xff9f);
    pub const KP_MULTIPLY: Keysym = Keysym(0xffaa);
    pub const KP_ADD: Keysym = Keysym(0xffab);
    pub const KP_SEPARATOR: Keysym = Keysym(0xffac);
    pub const KP_SUBTRACT: Keysym = Keysym(0xffad);
    pub const KP_DECIMAL: Keysym = Keysym(0xffae);
    pub const KP_DIVIDE: Keysym = Keysym(0xffaf);
    pub const KP_0: Keysym = Keysym(0xffb0);
    pub const KP_1: Keysym = Keysym(0xffb1);
    pub const KP_2: Keysym = Keysym(0xffb2);
    pub const KP_3: Keysym = Keysym(0xffb3);
    pub const KP_4: Keysym = Keysym(0xffb4);
    pub const KP_5: Keysym = Keysym(0xffb5);
    pub const KP_6: Keysym = Keysym(0xffb6);
    pub const KP_7: Keysym = Keysym(0xffb7);
    pub const KP_8: Keysym = Keysym(0xffb8);
    pub const KP_9: Keysym = Keysym(0xffb9);
    pub const KP_EQUAL: Keysym = Keysym(0xffbd);

    pub const F1: Keysym = Keysym(0xffbe);
    pub const F2: Keysym = Keysym(0xffbf);
    pub const F3: Keysym = Keysym(0xffc0);
    pub const F4: Keysym = Keysym(0xffc1);
    pub const F5: Keysym = Keysym(0xffc2);
    pub const F6: Keysym = Keysym(0xffc3);
    pub const F7: Keysym = Keysym(0xffc4);
    pub const F8: Keysym = Keysym(0xffc5);
    pub const F9: Keysym = Keysym(0xffc6);
    pub const F10: Keysym = Keysym(0xffc7);
    pub const F11: Keysym = Keysym(0xffc8);
    pub const F12: Keysym = Keysym(0xffc9);
    pub const F13: Keysym = Keysym(0xffca);
    pub const F14: Keysym = Keysym(0xffcb);
    pub const F15: Keysym = Keysym(0xffcc);
    pub const F16: Keysym = Keysym(0xffcd);
    pub const F17: Keysym = Keysym(0xffce);
    pub const F18: Keysym = Keysym(0xffcf);
    pub const F19: Keysym = Keysym(0xffd0);
    pub const F20: Keysym = Keysym(0xffd1);

    /// Function key `F<n>` for `n` in 1..=20.
    pub const fn function(n: u32) -> Option<Keysym> {
        if n >= 1 && n <= 20 { Some(Keysym(Self::F1.0 + n - 1)) } else { None }
    }
}

impl From<char> for Keysym {
    /// Keysym of a character: Latin-1 keysyms are the code point, other
    /// characters use the XKB Unicode keysym range.
    fn from(c: char) -> Self {
        let c = c as u32;
        if c < 0x100 { Keysym(c) } else { Keysym(0x0100_0000 | c) }
    }
}
