//! Golden snapshot tests: replay tests/golden/scenarios/*.scn and compare
//! the transcripts with the recorded `.out` files. The transcripts were
//! originally recorded from the C implementation of libtsm; see README.md.
//!
//! Environment:
//! * `GOLDEN_FILTER=substr`: only run matching scenarios.
//! * `GOLDEN_BLESS=1`: rewrite the `.out` files of failing scenarios (review
//!   the diff before committing).

mod dump;
mod script;

use std::fmt::Write;
use std::path::{Path, PathBuf};

use script::{Token, first_codepoint, tokenize};
use tsm::{
    Handler, Keysym, Leds, Modifiers, MouseButton, MouseEvent, MouseEventKind, MouseModifiers, MouseTracking, Palette,
    ScreenFlags, Vte,
};

/// Records everything the vte reports, in transcript format.
#[derive(Default)]
pub struct Recorder {
    out: String,
    writes: Vec<u8>,
}

impl Recorder {
    fn flush(&mut self) {
        if !self.writes.is_empty() {
            self.out.push_str("< write ");
            dump::escape(&mut self.out, &self.writes);
            self.out.push('\n');
            self.writes.clear();
        }
    }
}

impl Handler for Recorder {
    fn write(&mut self, data: &[u8]) {
        self.writes.extend_from_slice(data);
    }

    fn osc(&mut self, data: &[u8]) {
        self.flush();
        self.out.push_str("< osc ");
        dump::escape(&mut self.out, data);
        self.out.push('\n');
    }

    fn bell(&mut self) {
        self.flush();
        self.out.push_str("< bell\n");
    }

    fn leds(&mut self, leds: Leds) {
        self.flush();
        let names: Vec<&str> = [(Leds::SCROLL_LOCK, "scroll"), (Leds::NUM_LOCK, "num"), (Leds::CAPS_LOCK, "caps")]
            .iter()
            .filter(|(l, _)| leds.contains(*l))
            .map(|(_, n)| *n)
            .collect();
        let names = if names.is_empty() { "none".to_string() } else { names.join("+") };
        writeln!(self.out, "< led {names}").unwrap();
    }

    fn mouse_mode(&mut self, tracking: MouseTracking, pixels: bool) {
        self.flush();
        writeln!(self.out, "< mouse-cb mode={} pixels={}", tracking.code(), pixels as u8).unwrap();
    }
}

struct Runner {
    vte: Vte<Recorder>,
    palette_name: Option<String>,
    custom_palette: Option<Palette>,
}

const KEYSYMS: &[(&str, u32)] = &[
    ("NoSymbol", 0x0),
    ("space", 0x20),
    ("asciitilde", 0x7e),
    ("backslash", 0x5c),
    ("bar", 0x7c),
    ("braceleft", 0x7b),
    ("braceright", 0x7d),
    ("bracketleft", 0x5b),
    ("bracketright", 0x5d),
    ("grave", 0x60),
    ("question", 0x3f),
    ("slash", 0x2f),
    ("BackSpace", 0xff08),
    ("Tab", 0xff09),
    ("ISO_Left_Tab", 0xfe20),
    ("Linefeed", 0xff0a),
    ("Clear", 0xff0b),
    ("Pause", 0xff13),
    ("Scroll_Lock", 0xff14),
    ("Sys_Req", 0xff15),
    ("Escape", 0xff1b),
    ("Return", 0xff0d),
    ("Find", 0xff68),
    ("Insert", 0xff63),
    ("Delete", 0xffff),
    ("Select", 0xff60),
    ("Page_Up", 0xff55),
    ("Page_Down", 0xff56),
    ("Home", 0xff50),
    ("End", 0xff57),
    ("Up", 0xff52),
    ("Down", 0xff54),
    ("Left", 0xff51),
    ("Right", 0xff53),
    ("KP_0", 0xffb0),
    ("KP_1", 0xffb1),
    ("KP_2", 0xffb2),
    ("KP_3", 0xffb3),
    ("KP_4", 0xffb4),
    ("KP_5", 0xffb5),
    ("KP_6", 0xffb6),
    ("KP_7", 0xffb7),
    ("KP_8", 0xffb8),
    ("KP_9", 0xffb9),
    ("KP_Add", 0xffab),
    ("KP_Decimal", 0xffae),
    ("KP_Delete", 0xff9f),
    ("KP_Divide", 0xffaf),
    ("KP_Down", 0xff99),
    ("KP_End", 0xff9c),
    ("KP_Enter", 0xff8d),
    ("KP_Equal", 0xffbd),
    ("KP_F1", 0xff91),
    ("KP_F2", 0xff92),
    ("KP_F3", 0xff93),
    ("KP_F4", 0xff94),
    ("KP_Home", 0xff95),
    ("KP_Insert", 0xff9e),
    ("KP_Left", 0xff96),
    ("KP_Multiply", 0xffaa),
    ("KP_Page_Down", 0xff9b),
    ("KP_Page_Up", 0xff9a),
    ("KP_Right", 0xff98),
    ("KP_Separator", 0xffac),
    ("KP_Space", 0xff80),
    ("KP_Subtract", 0xffad),
    ("KP_Tab", 0xff89),
    ("KP_Up", 0xff97),
];

fn parse_uint(t: &Token) -> Result<u64, String> {
    let s = t.text();
    let r = match s.strip_prefix("0x") {
        Some(h) => u64::from_str_radix(h, 16),
        None => s.parse(),
    };
    r.map_err(|_| format!("bad number '{s}'"))
}

fn parse_keysym(t: &Token) -> Result<u32, String> {
    let s = t.text();
    if let Some((_, v)) = KEYSYMS.iter().find(|(n, _)| *n == s) {
        return Ok(*v);
    }
    if let Some(k) = s.strip_prefix('F').and_then(|n| n.parse().ok()).and_then(Keysym::function) {
        return Ok(k.0);
    }
    if s.len() == 1 && s.as_bytes()[0] > 0x20 && s.as_bytes()[0] < 0x7f {
        return Ok(s.as_bytes()[0] as u32);
    }
    if s.starts_with("0x") {
        return parse_uint(t).map(|v| v as u32);
    }
    Err(format!("unknown keysym '{s}'"))
}

fn parse_pair(s: &str) -> Result<(u32, u32), String> {
    let (x, y) = s.split_once(',').ok_or("expected X,Y")?;
    Ok((x.parse().map_err(|_| "expected X,Y")?, y.parse().map_err(|_| "expected X,Y")?))
}

fn parse_mask<T: Copy + std::ops::BitOr<Output = T>>(s: &str, names: &[(&str, T)], empty: T) -> Result<T, String> {
    if s == "none" {
        return Ok(empty);
    }
    s.split('+').try_fold(empty, |acc, n| {
        names.iter().find(|(name, _)| *name == n).map(|(_, v)| acc | *v).ok_or(format!("unknown name '{n}'"))
    })
}

fn need(t: &[Token], min: usize, max: usize) -> Result<(), String> {
    if t.len() - 1 < min || t.len() - 1 > max { Err("wrong number of arguments".into()) } else { Ok(()) }
}

impl Runner {
    fn new() -> Self {
        let mut vte = Vte::new(Recorder::default());
        vte.screen_mut().resize(80, 24);
        Runner { vte, palette_name: None, custom_palette: None }
    }

    fn out(&mut self) -> &mut String {
        &mut self.vte.handler_mut().out
    }

    fn apply_palette(&mut self) {
        let palette = match self.palette_name.as_deref() {
            None => Palette::LEGACY,
            Some("custom") => self.custom_palette.unwrap_or(Palette::LEGACY),
            Some(name) => Palette::by_name(name).unwrap_or(Palette::LEGACY),
        };
        self.vte.set_palette(palette);
    }

    fn run(&mut self, t: &[Token]) -> Result<(), String> {
        let cmd = t[0].text().to_string();
        let vte = &mut self.vte;
        match cmd.as_str() {
            "resize" => {
                need(t, 2, 2)?;
                vte.screen_mut().resize(parse_uint(&t[1])? as usize, parse_uint(&t[2])? as usize);
            }
            "in" | "in1" => {
                need(t, 1, usize::MAX)?;
                for tok in &t[1..] {
                    if !tok.quoted || tok.key.is_some() {
                        return Err("expected string".into());
                    }
                    if cmd == "in" {
                        vte.input(&tok.val);
                    } else {
                        for b in &tok.val {
                            vte.input(std::slice::from_ref(b));
                        }
                    }
                }
            }
            "hex" => {
                need(t, 1, usize::MAX)?;
                let bytes: Result<Vec<u8>, _> = t[1..].iter().map(|b| u8::from_str_radix(b.text(), 16)).collect();
                vte.input(&bytes.map_err(|_| "expected hex byte")?);
            }
            "key" => {
                need(t, 1, 4)?;
                let keysym = parse_keysym(&t[1])?;
                let (mut ascii, mut mods, mut uni) = (0, Modifiers::empty(), None);
                for tok in &t[2..] {
                    match tok.key.as_deref() {
                        Some("ascii") => ascii = parse_keysym(tok)?,
                        Some("mods") => mods = parse_mask(tok.text(), Modifiers::NAMED_LOWER, Modifiers::empty())?,
                        Some("uni") if tok.quoted => {
                            let (c, n) = first_codepoint(&tok.val)?;
                            if n != tok.val.len() {
                                return Err("uni= takes a single character".into());
                            }
                            uni = char::from_u32(c);
                        }
                        Some("uni") if tok.text() == "none" => uni = None,
                        Some("uni") => {
                            let hex = tok.text().strip_prefix("U+").ok_or("bad uni= value")?;
                            uni = char::from_u32(u32::from_str_radix(hex, 16).map_err(|_| "bad uni= value")?);
                        }
                        _ => return Err("unknown key option".into()),
                    }
                }
                let r = vte.handle_keyboard(Keysym(keysym), Keysym(ascii), mods, uni);
                vte.handler_mut().flush();
                writeln!(self.out(), "< ret {r}").unwrap();
            }
            "type" => {
                need(t, 1, 1)?;
                for c in String::from_utf8_lossy(&t[1].val).chars() {
                    let ascii = if c.is_ascii() { Keysym(c as u32) } else { Keysym::NO_SYMBOL };
                    vte.handle_keyboard(Keysym::from(c), ascii, Modifiers::empty(), Some(c));
                }
            }
            "mouse" => {
                need(t, 3, 5)?;
                let kind = match t[1].text() {
                    "press" => MouseEventKind::PRESSED,
                    "release" => MouseEventKind::RELEASED,
                    "move" => MouseEventKind::MOVED,
                    _ => MouseEventKind::from_bits_retain(parse_uint(&t[1])? as u32),
                };
                let button = match t[2].text() {
                    "left" => MouseButton::LEFT,
                    "middle" => MouseButton::MIDDLE,
                    "right" => MouseButton::RIGHT,
                    "wheelup" => MouseButton::WHEEL_UP,
                    "wheeldown" => MouseButton::WHEEL_DOWN,
                    _ => MouseButton(parse_uint(&t[2])? as u8),
                };
                let mut ev = MouseEvent { kind, button, ..Default::default() };
                let mut have_cell = false;
                for tok in &t[3..] {
                    match tok.key.as_deref() {
                        Some("cell") => {
                            (ev.cell_x, ev.cell_y) = parse_pair(tok.text())?;
                            have_cell = true;
                        }
                        Some("px") => (ev.pixel_x, ev.pixel_y) = parse_pair(tok.text())?,
                        Some("mods") => {
                            let names = [
                                ("shift", MouseModifiers::SHIFT),
                                ("meta", MouseModifiers::META),
                                ("ctrl", MouseModifiers::CTRL),
                            ];
                            ev.modifiers = parse_mask(tok.text(), &names, MouseModifiers::empty())?;
                        }
                        _ => return Err("unknown mouse option".into()),
                    }
                }
                if !have_cell {
                    return Err("mouse needs cell=X,Y".into());
                }
                let r = vte.handle_mouse(&ev);
                vte.handler_mut().flush();
                writeln!(self.out(), "< ret {r}").unwrap();
            }
            "paste" => {
                need(t, 1, 1)?;
                vte.paste(&t[1].val);
            }
            "sel-start" | "sel-target" | "sel-word" => {
                need(t, 2, 2)?;
                let (x, y) = (parse_uint(&t[1])? as usize, parse_uint(&t[2])? as usize);
                let s = vte.screen_mut();
                match cmd.as_str() {
                    "sel-start" => s.selection_start(x, y),
                    "sel-target" => s.selection_target(x, y),
                    _ => s.selection_word(x, y),
                }
            }
            "sel-reset" => vte.screen_mut().selection_reset(),
            "sel-copy" => {
                let copy = vte.screen_mut().selection_copy();
                let out = self.out();
                match copy {
                    None => out.push_str("< copy none\n"),
                    Some(s) => {
                        out.push_str("< copy ");
                        dump::escape(out, s.as_bytes());
                        out.push('\n');
                    }
                }
            }
            "sb-max" => vte.screen_mut().set_max_scrollback(parse_uint(&t[1])? as usize),
            "sb-clear" => vte.screen_mut().clear_scrollback(),
            "sb-up" => vte.screen_mut().scrollback_up(parse_uint(&t[1])? as usize),
            "sb-down" => vte.screen_mut().scrollback_down(parse_uint(&t[1])? as usize),
            "sb-page-up" => vte.screen_mut().scrollback_page_up(parse_uint(&t[1])? as usize),
            "sb-page-down" => vte.screen_mut().scrollback_page_down(parse_uint(&t[1])? as usize),
            "sb-reset" => vte.screen_mut().scrollback_reset(),
            "palette" => {
                need(t, 1, 1)?;
                self.palette_name = (t[1].text() != "none").then(|| t[1].text().to_string());
                self.apply_palette();
            }
            "custom-palette" => {
                if t.len() == 2 && t[1].text() == "none" {
                    self.custom_palette = None;
                } else {
                    need(t, 18, 18)?;
                    let mut p = Palette::LEGACY;
                    for (i, tok) in t[1..].iter().enumerate() {
                        let v = u32::from_str_radix(tok.text(), 16).map_err(|_| "expected RRGGBB")?;
                        p.0[i] = tsm::Rgb::new((v >> 16) as u8, (v >> 8) as u8, v as u8);
                    }
                    self.custom_palette = Some(p);
                }
                self.apply_palette();
            }
            "bs-delete" => vte.set_backspace_sends_delete(t[1].text() == "on"),
            "screen-set" | "screen-reset" => {
                need(t, 1, usize::MAX)?;
                let mut flags = ScreenFlags::empty();
                for tok in &t[1..] {
                    let (_, f) = ScreenFlags::NAMED
                        .iter()
                        .find(|(n, _)| *n == tok.text())
                        .ok_or(format!("unknown screen flag '{}'", tok.text()))?;
                    flags |= *f;
                }
                if cmd == "screen-set" {
                    vte.screen_mut().set_flags(flags);
                } else {
                    vte.screen_mut().reset_flags(flags);
                }
            }
            "margins" => {
                need(t, 2, 2)?;
                vte.screen_mut().set_margins(parse_uint(&t[1])? as usize, parse_uint(&t[2])? as usize);
            }
            "reset" => vte.reset(),
            "hard-reset" => vte.hard_reset(),
            "dump" => {
                let sections: Vec<&str> = if t.len() == 1 {
                    vec!["state", "text", "attrs"]
                } else {
                    t[1..].iter().map(|t| t.text()).collect()
                };
                let mut out = String::new();
                for s in sections {
                    match s {
                        "state" => dump::state(&mut out, vte),
                        "text" => dump::text(&mut out, vte.screen()),
                        "attrs" => dump::attrs(&mut out, vte),
                        "render" => dump::render(&mut out, vte),
                        _ => return Err(format!("unknown dump section '{s}'")),
                    }
                }
                self.out().push_str(&out);
            }
            "width-table" => dump::width_table(self.out()),
            _ => return Err(format!("unknown command '{cmd}'")),
        }
        self.vte.handler_mut().flush();
        Ok(())
    }
}

/// Lower-case modifier names used by scenarios.
trait ModifierNames {
    const NAMED_LOWER: &'static [(&'static str, Modifiers)];
}

impl ModifierNames for Modifiers {
    const NAMED_LOWER: &'static [(&'static str, Modifiers)] = &[
        ("shift", Modifiers::SHIFT),
        ("lock", Modifiers::LOCK),
        ("ctrl", Modifiers::CONTROL),
        ("alt", Modifiers::ALT),
        ("logo", Modifiers::LOGO),
    ];
}

fn run_scenario(path: &Path) -> Result<String, String> {
    let src = std::fs::read_to_string(path).map_err(|e| e.to_string())?;
    let mut runner = Runner::new();
    for (n, line) in src.lines().enumerate() {
        let line = line.trim_matches([' ', '\t', '\r']);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        writeln!(runner.out(), "> {line}").unwrap();
        let toks = tokenize(line).map_err(|e| format!("{}:{}: {e}", path.display(), n + 1))?;
        if !toks.is_empty() {
            runner.run(&toks).map_err(|e| format!("{}:{}: {e}", path.display(), n + 1))?;
        }
    }
    Ok(std::mem::take(runner.out()))
}

fn first_diff(expected: &str, actual: &str) -> String {
    let (e, a): (Vec<&str>, Vec<&str>) = (expected.lines().collect(), actual.lines().collect());
    let i = e.iter().zip(&a).position(|(x, y)| x != y).unwrap_or(e.len().min(a.len()));
    let from = i.saturating_sub(3);
    let mut s = String::new();
    for l in &e[from..i] {
        writeln!(s, "      {l}").unwrap();
    }
    for l in e.iter().skip(i).take(4) {
        writeln!(s, "    - {l}").unwrap();
    }
    for l in a.iter().skip(i).take(4) {
        writeln!(s, "    + {l}").unwrap();
    }
    s
}

#[test]
fn golden() {
    let dir = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/golden/scenarios");
    let filter = std::env::var("GOLDEN_FILTER").ok();
    let bless = std::env::var_os("GOLDEN_BLESS").is_some_and(|v| v == "1");

    let mut scenarios: Vec<PathBuf> = std::fs::read_dir(&dir)
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| p.extension().is_some_and(|e| e == "scn"))
        .collect();
    scenarios.sort();

    let (mut passed, mut blessed, mut errors) = (0, 0, Vec::new());
    for path in &scenarios {
        let name = path.file_stem().unwrap().to_str().unwrap().to_string();
        if filter.as_ref().is_some_and(|f| !name.contains(f.as_str())) {
            continue;
        }
        let expected_path = dir.join(format!("{name}.out"));
        let expected = std::fs::read_to_string(&expected_path).unwrap_or_default();

        let detail = match std::panic::catch_unwind(|| run_scenario(path)) {
            Ok(Ok(actual)) if actual == expected => {
                passed += 1;
                continue;
            }
            Ok(Ok(actual)) if bless => {
                std::fs::write(&expected_path, actual).unwrap();
                blessed += 1;
                continue;
            }
            Ok(Ok(actual)) => first_diff(&expected, &actual),
            Ok(Err(e)) => format!("    error: {e}\n"),
            Err(_) => "    panicked\n".to_string(),
        };
        errors.push(format!("{name}: transcript differs from {}\n{detail}", expected_path.display()));
    }

    eprintln!("{passed} scenarios passed, {blessed} blessed");
    if !errors.is_empty() {
        panic!("{} golden failures:\n{}", errors.len(), errors.join("\n"));
    }
}
