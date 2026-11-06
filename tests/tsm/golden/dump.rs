//! Transcript dumps, see README.md for the format.

use std::fmt::Write;

use tsm::{Attr, AttrFlags, Rgb, Screen, ScreenFlags, Vte, VteFlags};

use super::Recorder;

/// Names used in transcripts (the C macro suffixes), in bit order.
const VTE_FLAG_NAMES: &[(&str, VteFlags)] = &[
    ("CURSOR_KEY_MODE", VteFlags::CURSOR_KEY_MODE),
    ("KEYPAD_APPLICATION_MODE", VteFlags::KEYPAD_APPLICATION_MODE),
    ("LINE_FEED_NEW_LINE_MODE", VteFlags::LINE_FEED_NEW_LINE_MODE),
    ("8BIT_MODE", VteFlags::EIGHT_BIT_MODE),
    ("7BIT_MODE", VteFlags::SEVEN_BIT_MODE),
    ("USE_C1", VteFlags::USE_C1),
    ("KEYBOARD_ACTION_MODE", VteFlags::KEYBOARD_ACTION_MODE),
    ("INSERT_REPLACE_MODE", VteFlags::INSERT_REPLACE_MODE),
    ("SEND_RECEIVE_MODE", VteFlags::SEND_RECEIVE_MODE),
    ("TEXT_CURSOR_MODE", VteFlags::TEXT_CURSOR_MODE),
    ("INVERSE_SCREEN_MODE", VteFlags::INVERSE_SCREEN_MODE),
    ("ORIGIN_MODE", VteFlags::ORIGIN_MODE),
    ("AUTO_WRAP_MODE", VteFlags::AUTO_WRAP_MODE),
    ("AUTO_REPEAT_MODE", VteFlags::AUTO_REPEAT_MODE),
    ("NATIONAL_CHARSET_MODE", VteFlags::NATIONAL_CHARSET_MODE),
    ("BACKGROUND_COLOR_ERASE_MODE", VteFlags::BACKGROUND_COLOR_ERASE_MODE),
    ("PREPEND_ESCAPE", VteFlags::PREPEND_ESCAPE),
    ("TITE_INHIBIT_MODE", VteFlags::TITE_INHIBIT_MODE),
];

pub fn escape(out: &mut String, data: &[u8]) {
    out.push('"');
    for &c in data {
        match c {
            0x1b => out.push_str("\\e"),
            0x07 => out.push_str("\\a"),
            0x08 => out.push_str("\\b"),
            0x09 => out.push_str("\\t"),
            0x0a => out.push_str("\\n"),
            0x0d => out.push_str("\\r"),
            b'\\' => out.push_str("\\\\"),
            b'"' => out.push_str("\\\""),
            0x20..0x7f => out.push(c as char),
            _ => write!(out, "\\x{c:02x}").unwrap(),
        }
    }
    out.push('"');
}

fn rgb(c: Rgb) -> String {
    format!("{:02x}{:02x}{:02x}", c.r, c.g, c.b)
}

fn put_flags<T: Copy>(out: &mut String, names: &[(&str, T)], contains: impl Fn(T) -> bool) {
    let set: Vec<&str> = names.iter().filter(|(_, f)| contains(*f)).map(|(n, _)| *n).collect();
    if set.is_empty() {
        out.push('-');
    } else {
        out.push_str(&set.join("|"));
    }
}

fn attr_flags(flags: AttrFlags) -> String {
    let letters = [
        (AttrFlags::BOLD, 'B'),
        (AttrFlags::DIM, 'D'),
        (AttrFlags::ITALIC, 'I'),
        (AttrFlags::UNDERLINE, 'U'),
        (AttrFlags::BLINK, 'K'),
        (AttrFlags::INVERSE, 'R'),
        (AttrFlags::PROTECT, 'P'),
    ];
    let s: String = letters.iter().filter(|(f, _)| flags.contains(*f)).map(|(_, l)| *l).collect();
    if s.is_empty() { "-".into() } else { s }
}

/// Characters printed literally in text grids.
fn printable(c: char) -> bool {
    let c = c as u32;
    if (0x20..0x7f).contains(&c) {
        return true;
    }
    if c < 0xa0 {
        return false;
    }
    if c & 0xfffe == 0xfffe || (0xfdd0..=0xfdef).contains(&c) {
        return false;
    }
    c != 0xb7 && c != 0xad
}

pub fn state(out: &mut String, vte: &Vte<Recorder>) {
    let s = vte.screen();
    let (cx, cy) = s.cursor();
    let (mt, mb) = s.margins();
    let def = vte.default_attr();
    let erase = s.default_attr();

    out.push_str("state\n");
    writeln!(out, "  size {}x{} cursor {cx},{cy}", s.width(), s.height()).unwrap();
    writeln!(out, "  cursor-style {} blink {}", s.cursor_style().code(), s.blink_rate_ms()).unwrap();
    writeln!(out, "  margins {mt}-{mb}").unwrap();
    let tabs: Vec<String> = s.tab_stops().map(|t| t.to_string()).collect();
    writeln!(out, "  tabs {}", if tabs.is_empty() { "-".into() } else { tabs.join(",") }).unwrap();
    out.push_str("  screen-flags ");
    put_flags(out, ScreenFlags::NAMED, |f| s.flags().contains(f));
    out.push_str("\n  vte-flags ");
    put_flags(out, VTE_FLAG_NAMES, |f| vte.flags().contains(f));
    writeln!(out, "\n  mouse-mode {}", vte.mouse_protocol().code()).unwrap();
    writeln!(out, "  sb count={} pos={} max={}", s.scrollback_len(), s.scrollback_pos(), s.max_scrollback()).unwrap();
    writeln!(out, "  default {}/{}", rgb(def.fg), rgb(def.bg)).unwrap();
    writeln!(out, "  erase {}/{} {}", rgb(erase.fg), rgb(erase.bg), attr_flags(erase.flags)).unwrap();
}

pub fn text(out: &mut String, s: &Screen) {
    let w = s.width();
    let is_tail =
        |row: &[tsm::Cell], x: usize| x > 0 && row[x].width == 0 && row[x].ch == '\0' && row[x - 1].width == 2;
    let is_odd = |row: &[tsm::Cell], x: usize| {
        let c = &row[x];
        if c.ch != '\0' && !printable(c.ch) {
            return true;
        }
        match c.width {
            0 => !is_tail(row, x),
            1 => false,
            2 => x + 1 >= w || row[x + 1].width != 0,
            _ => true,
        }
    };

    let mut odd = false;
    out.push_str("text\n");
    for y in 0..s.height() {
        let row = s.row(y).unwrap();
        write!(out, "  {y:2}|").unwrap();
        for x in 0..w {
            odd |= is_odd(row, x);
            if is_tail(row, x) {
                continue;
            }
            let c = row[x].ch;
            out.push(match c {
                '\0' => '·',
                c if printable(c) => c,
                _ => '\u{fffd}',
            });
        }
        out.push_str("|\n");
    }

    if odd {
        out.push_str("cells\n");
        for y in 0..s.height() {
            let row = s.row(y).unwrap();
            for x in 0..w {
                if is_odd(row, x) {
                    writeln!(out, "  {y},{x} U+{:04X} w={}", row[x].ch as u32, row[x].width).unwrap();
                }
            }
        }
    }
}

fn runs<T: PartialEq>(out: &mut String, y: usize, cells: &[T], skip: impl Fn(&T) -> bool, desc: impl Fn(&T) -> String) {
    let mut x = 0;
    while x < cells.len() {
        let mut end = x + 1;
        while end < cells.len() && cells[end] == cells[x] {
            end += 1;
        }
        if !skip(&cells[x]) {
            if end - 1 == x {
                write!(out, "  {y}: {x} ").unwrap();
            } else {
                write!(out, "  {y}: {x}-{} ", end - 1).unwrap();
            }
            writeln!(out, "{}", desc(&cells[x])).unwrap();
        }
        x = end;
    }
}

pub fn attrs(out: &mut String, vte: &Vte<Recorder>) {
    let s = vte.screen();
    let def = Attr { flags: AttrFlags::empty(), ..vte.default_attr() };
    out.push_str("attrs\n");
    for y in 0..s.height() {
        let attrs: Vec<Attr> = s.row(y).unwrap().iter().map(|c| c.attr).collect();
        runs(out, y, &attrs, |a| *a == def, |a| format!("{}/{} {}", rgb(a.fg), rgb(a.bg), attr_flags(a.flags)));
    }
}

pub fn render(out: &mut String, vte: &Vte<Recorder>) {
    let s = vte.screen();
    let w = s.width();
    let cells = s.render();
    let def = vte.default_attr();

    out.push_str("render\n");
    for y in 0..s.height() {
        write!(out, "  {y:2}|").unwrap();
        for c in &cells[y * w..(y + 1) * w] {
            out.push(if printable(c.ch) { c.ch } else { '\u{fffd}' });
        }
        out.push_str("|\n");
    }
    for y in 0..s.height() {
        let row: Vec<(Rgb, Rgb, AttrFlags)> = cells[y * w..(y + 1) * w].iter().map(|c| (c.fg, c.bg, c.flags)).collect();
        runs(
            out,
            y,
            &row,
            |r| *r == (def.fg, def.bg, AttrFlags::empty()),
            |(fg, bg, f)| {
                let letters = [
                    (AttrFlags::BOLD, 'B'),
                    (AttrFlags::ITALIC, 'I'),
                    (AttrFlags::UNDERLINE, 'U'),
                    (AttrFlags::BLINK, 'K'),
                ];
                let l: String = letters.iter().filter(|(x, _)| f.contains(*x)).map(|(_, c)| *c).collect();
                format!("{}/{} {}", rgb(*fg), rgb(*bg), if l.is_empty() { "-".into() } else { l })
            },
        );
    }
}

pub fn width_table(out: &mut String) {
    let width = |c: u32| char::from_u32(c).map_or(0, tsm::char_width);
    let (mut start, mut prev, mut cur) = (0u32, 0u32, width(0));
    for c in (1..=0x10ffffu32).filter(|c| !(0xd800..=0xdfff).contains(c)) {
        let w = width(c);
        if w != cur {
            writeln!(out, "< width {start:04X}-{prev:04X} {cur}").unwrap();
            start = c;
            cur = w;
        }
        prev = c;
    }
    writeln!(out, "< width {start:04X}-{prev:04X} {cur}").unwrap();
}
