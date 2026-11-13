// SPDX-License-Identifier: MIT
//
// C bindings for libtsm, used by kmscon.
//
// This exposes a small C API (see tsm.h) on top of `Vte`. A single opaque
// `struct tsm_vte` owns both the parser and the screen.

#![allow(non_camel_case_types)]
#![allow(clippy::missing_safety_doc)]

use std::ffi::{CStr, c_char, c_int, c_uint, c_void};
use std::{mem, ptr, slice};

use crate::{
    Attr, CursorStyle, Handler, Keysym, Leds, Modifiers, MouseButton, MouseEvent, MouseEventKind, MouseModifiers,
    MouseProtocol, MouseTracking, Palette, RenderCell, Rgb, ScreenFlags, Vte,
};

const TSM_INVALID: u32 = 0xffff_ffff;
const EINVAL: c_int = 22;
const ENOENT: c_int = 2;
const ENOMEM: c_int = 12;
const COLOR_NUM: usize = 18;

unsafe extern "C" {
    fn malloc(size: usize) -> *mut c_void;
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct tsm_color {
    r: u8,
    g: u8,
    b: u8,
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct tsm_attr {
    fg: tsm_color,
    bg: tsm_color,
    flags: u8,
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct tsm_cell {
    ch: u32,
    fg: tsm_color,
    bg: tsm_color,
    attr: u8,
    reserved: u8,
}

const _: () = assert!(mem::size_of::<tsm_color>() == 3);
const _: () = assert!(mem::size_of::<tsm_attr>() == 7);
const _: () = assert!(mem::size_of::<tsm_cell>() == 12 && mem::align_of::<tsm_cell>() == 4);

#[repr(C)]
#[derive(Copy, Clone)]
pub struct tsm_callbacks {
    write: Option<unsafe extern "C" fn(data: *mut c_void, buf: *const c_char, len: usize)>,
    osc: Option<unsafe extern "C" fn(data: *mut c_void, s: *const c_char, len: usize)>,
    bell: Option<unsafe extern "C" fn(data: *mut c_void)>,
    leds: Option<unsafe extern "C" fn(data: *mut c_void, leds: c_uint)>,
    mouse_mode: Option<unsafe extern "C" fn(data: *mut c_void, enabled: bool, pixels: bool)>,
}

impl From<Rgb> for tsm_color {
    fn from(c: Rgb) -> Self {
        tsm_color { r: c.r, g: c.g, b: c.b }
    }
}

impl From<Attr> for tsm_attr {
    fn from(a: Attr) -> Self {
        tsm_attr { fg: a.fg.into(), bg: a.bg.into(), flags: a.flags.bits() }
    }
}

impl From<&RenderCell> for tsm_cell {
    fn from(c: &RenderCell) -> Self {
        tsm_cell { ch: c.ch as u32, fg: c.fg.into(), bg: c.bg.into(), attr: c.flags.bits(), reserved: 0 }
    }
}

/// Events that may call back into the vte from C. They are queued while the
/// vte is borrowed and dispatched once the Rust call returned.
enum Event {
    Osc(Vec<u8>),
    Bell,
    Leds(Leds),
    MouseMode(bool, bool),
}

struct CHandler {
    cb: tsm_callbacks,
    data: *mut c_void,
    events: Vec<Event>,
}

fn mouse_enabled(protocol: MouseProtocol, tracking: MouseTracking) -> bool {
    protocol != MouseProtocol::None && tracking != MouseTracking::Disabled
}

impl Handler for CHandler {
    fn write(&mut self, data: &[u8]) {
        // the pty write does not touch the vte, so it is safe to call now
        if let Some(f) = self.cb.write {
            unsafe { f(self.data, data.as_ptr().cast(), data.len()) };
        }
    }

    fn osc(&mut self, data: &[u8]) {
        let mut s = data.to_vec();
        s.push(0);
        self.events.push(Event::Osc(s));
    }

    fn bell(&mut self) {
        self.events.push(Event::Bell);
    }

    fn leds(&mut self, leds: Leds) {
        self.events.push(Event::Leds(leds));
    }

    fn mouse_mode(&mut self, tracking: MouseTracking, pixels: bool) {
        // the protocol is not passed here, it is resolved in dispatch()
        let enabled = tracking != MouseTracking::Disabled;
        self.events.push(Event::MouseMode(enabled, pixels));
    }
}

pub struct tsm_vte {
    vte: Vte<CHandler>,
    cells: Vec<RenderCell>,
    out: Vec<tsm_cell>,
}

/// Calls the queued C callbacks. No Rust reference to the vte may be alive
/// while this runs, as callbacks are allowed to call back into the API.
unsafe fn dispatch(vte: *mut tsm_vte) {
    loop {
        let (ev, cb, data, mouse_on) = {
            let t = unsafe { &mut *vte };
            let h = t.vte.handler_mut();
            if h.events.is_empty() {
                return;
            }
            let ev = h.events.remove(0);
            let (cb, data) = (h.cb, h.data);
            (ev, cb, data, mouse_enabled(t.vte.mouse_protocol(), t.vte.mouse_tracking()))
        };
        unsafe {
            match ev {
                Event::Osc(s) => {
                    if let Some(f) = cb.osc {
                        f(data, s.as_ptr().cast(), s.len() - 1);
                    }
                }
                Event::Bell => {
                    if let Some(f) = cb.bell {
                        f(data);
                    }
                }
                Event::Leds(l) => {
                    if let Some(f) = cb.leds {
                        f(data, l.bits());
                    }
                }
                Event::MouseMode(enabled, pixels) => {
                    if let Some(f) = cb.mouse_mode {
                        f(data, enabled && mouse_on, pixels);
                    }
                }
            }
        }
    }
}

unsafe fn bytes<'a>(buf: *const c_char, len: usize) -> &'a [u8] {
    if buf.is_null() || len == 0 { &[] } else { unsafe { slice::from_raw_parts(buf.cast(), len) } }
}

macro_rules! vte {
    ($p:expr) => {
        match unsafe { $p.as_mut() } {
            Some(t) => t,
            None => return Default::default(),
        }
    };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_new(out: *mut *mut tsm_vte, cb: *const tsm_callbacks, data: *mut c_void) -> c_int {
    if out.is_null() || cb.is_null() {
        return -EINVAL;
    }
    let handler = CHandler { cb: unsafe { *cb }, data, events: Vec::new() };
    let t = Box::new(tsm_vte { vte: Vte::new(handler), cells: Vec::new(), out: Vec::new() });
    unsafe { *out = Box::into_raw(t) };
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_free(vte: *mut tsm_vte) {
    if !vte.is_null() {
        drop(unsafe { Box::from_raw(vte) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_input(vte: *mut tsm_vte, buf: *const c_char, len: usize) {
    vte!(vte).vte.input(unsafe { bytes(buf, len) });
    unsafe { dispatch(vte) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_paste(vte: *mut tsm_vte, buf: *const c_char, len: usize) {
    vte!(vte).vte.paste(unsafe { bytes(buf, len) });
    unsafe { dispatch(vte) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_hard_reset(vte: *mut tsm_vte) {
    vte!(vte).vte.hard_reset();
    unsafe { dispatch(vte) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_set_backspace_sends_delete(vte: *mut tsm_vte, enable: bool) {
    vte!(vte).vte.set_backspace_sends_delete(enable);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_set_palette(vte: *mut tsm_vte, name: *const c_char) -> c_int {
    let t = match unsafe { vte.as_mut() } {
        Some(t) => t,
        None => return -EINVAL,
    };
    let palette = if name.is_null() {
        Some(Palette::default())
    } else {
        unsafe { CStr::from_ptr(name) }.to_str().ok().and_then(Palette::by_name)
    };
    match palette {
        Some(p) => {
            t.vte.set_palette(p);
            0
        }
        None => -EINVAL,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_set_custom_palette(vte: *mut tsm_vte, rgb: *const [u8; 3]) -> c_int {
    let t = match unsafe { vte.as_mut() } {
        Some(t) => t,
        None => return -EINVAL,
    };
    if rgb.is_null() {
        return -EINVAL;
    }
    let src = unsafe { slice::from_raw_parts(rgb, COLOR_NUM) };
    let mut colors = [Rgb::default(); COLOR_NUM];
    for (dst, c) in colors.iter_mut().zip(src) {
        *dst = Rgb::new(c[0], c[1], c[2]);
    }
    t.vte.set_palette(Palette(colors));
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_get_default_attr(vte: *mut tsm_vte, out: *mut tsm_attr) {
    let t = vte!(vte);
    if let Some(out) = unsafe { out.as_mut() } {
        *out = t.vte.default_attr().into();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_handle_keyboard(
    vte: *mut tsm_vte,
    keysym: u32,
    ascii: u32,
    mods: c_uint,
    unicode: u32,
) -> bool {
    let unicode = if unicode == TSM_INVALID { None } else { char::from_u32(unicode) };
    let ascii = if ascii == TSM_INVALID { Keysym::NO_SYMBOL } else { Keysym(ascii) };
    let ret = vte!(vte).vte.handle_keyboard(Keysym(keysym), ascii, Modifiers::from_bits_retain(mods), unicode);
    unsafe { dispatch(vte) };
    ret
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_handle_mouse(
    vte: *mut tsm_vte,
    cell_x: c_uint,
    cell_y: c_uint,
    pixel_x: c_uint,
    pixel_y: c_uint,
    button: u8,
    event: c_uint,
    mods: u8,
) -> bool {
    let ev = MouseEvent {
        kind: MouseEventKind::from_bits_retain(event),
        button: MouseButton(button),
        modifiers: MouseModifiers::from_bits_retain(mods),
        cell_x,
        cell_y,
        pixel_x,
        pixel_y,
    };
    let ret = vte!(vte).vte.handle_mouse(&ev);
    unsafe { dispatch(vte) };
    ret
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_mouse_enabled(vte: *mut tsm_vte) -> bool {
    let t = vte!(vte);
    mouse_enabled(t.vte.mouse_protocol(), t.vte.mouse_tracking())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_resize(vte: *mut tsm_vte, cols: c_uint, rows: c_uint) {
    vte!(vte).vte.screen_mut().resize(cols as usize, rows as usize);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_get_width(vte: *mut tsm_vte) -> c_uint {
    vte!(vte).vte.screen().width() as c_uint
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_get_height(vte: *mut tsm_vte) -> c_uint {
    vte!(vte).vte.screen().height() as c_uint
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_set_max_scrollback(vte: *mut tsm_vte, max: c_uint) {
    vte!(vte).vte.screen_mut().set_max_scrollback(max as usize);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_sb_up(vte: *mut tsm_vte, num: c_uint) {
    vte!(vte).vte.screen_mut().scrollback_up(num as usize);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_sb_down(vte: *mut tsm_vte, num: c_uint) {
    vte!(vte).vte.screen_mut().scrollback_down(num as usize);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_sb_page_up(vte: *mut tsm_vte, num: c_uint) {
    vte!(vte).vte.screen_mut().scrollback_page_up(num as usize);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_sb_page_down(vte: *mut tsm_vte, num: c_uint) {
    vte!(vte).vte.screen_mut().scrollback_page_down(num as usize);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_sb_reset(vte: *mut tsm_vte) {
    vte!(vte).vte.screen_mut().scrollback_reset();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_render(vte: *mut tsm_vte) -> *const tsm_cell {
    let t = match unsafe { vte.as_mut() } {
        Some(t) => t,
        None => return ptr::null(),
    };
    t.vte.screen().render_into(&mut t.cells);
    t.out.clear();
    t.out.extend(t.cells.iter().map(tsm_cell::from));
    t.out.as_ptr()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_get_cursor(vte: *mut tsm_vte, x: *mut c_uint, y: *mut c_uint) -> bool {
    let t = vte!(vte);
    let s = t.vte.screen();
    let (cx, cy) = s.cursor();
    let cx = cx.min(s.width().saturating_sub(1));
    let cy = cy.min(s.height().saturating_sub(1));
    unsafe {
        if let Some(x) = x.as_mut() {
            *x = cx as c_uint;
        }
        if let Some(y) = y.as_mut() {
            *y = cy as c_uint;
        }
    }
    !s.flags().contains(ScreenFlags::HIDE_CURSOR) && s.scrollback_pos() == s.scrollback_len()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_get_cursor_style(vte: *mut tsm_vte) -> c_uint {
    match vte!(vte).vte.screen().cursor_style() {
        CursorStyle::Other(_) => 0,
        s => s.code() as c_uint,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_get_cursor_blink_rate_ms(vte: *mut tsm_vte) -> c_uint {
    vte!(vte).vte.screen().blink_rate_ms() as c_uint
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_selection_reset(vte: *mut tsm_vte) {
    vte!(vte).vte.screen_mut().selection_reset();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_selection_start(vte: *mut tsm_vte, x: c_uint, y: c_uint) {
    vte!(vte).vte.screen_mut().selection_start(x as usize, y as usize);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_selection_target(vte: *mut tsm_vte, x: c_uint, y: c_uint) {
    vte!(vte).vte.screen_mut().selection_target(x as usize, y as usize);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_selection_word(vte: *mut tsm_vte, x: c_uint, y: c_uint) {
    vte!(vte).vte.screen_mut().selection_word(x as usize, y as usize);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_vte_selection_copy(vte: *mut tsm_vte, out: *mut *mut c_char) -> c_int {
    let t = match unsafe { vte.as_mut() } {
        Some(t) => t,
        None => return -EINVAL,
    };
    if out.is_null() {
        return -EINVAL;
    }
    let s = match t.vte.screen_mut().selection_copy() {
        Some(s) => s,
        None => return -ENOENT,
    };
    if s.len() > c_int::MAX as usize {
        return -ENOMEM;
    }
    unsafe {
        let buf = malloc(s.len() + 1) as *mut u8;
        if buf.is_null() {
            return -ENOMEM;
        }
        ptr::copy_nonoverlapping(s.as_ptr(), buf, s.len());
        *buf.add(s.len()) = 0;
        *out = buf.cast();
    }
    s.len() as c_int
}

#[unsafe(no_mangle)]
pub extern "C" fn tsm_char_width(ucs4: u32) -> c_uint {
    char::from_u32(ucs4).map_or(0, crate::char_width) as c_uint
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tsm_ucs4_to_utf8(ucs4: u32, out: *mut c_char) -> usize {
    let (Some(c), false) = (char::from_u32(ucs4), out.is_null()) else {
        return 0;
    };
    let mut buf = [0u8; 4];
    let s = c.encode_utf8(&mut buf);
    unsafe { ptr::copy_nonoverlapping(s.as_ptr(), out.cast(), s.len()) };
    s.len()
}
