// SPDX-License-Identifier: MIT
//
// kmscon - /etc/issue display
//
// Copyright (c) 2025 Alberto Ruiz <aruiz@redhat.com>
//
// Reads the issue files, like agetty, and expands their escape sequences
// (see issue(5) and agetty(8)).

use std::ffi::{CStr, c_char};
use std::fs::{self, File};
use std::io::Read;
use std::path::Path;
use std::{ptr, slice};

use super::issue_network::AddrBook;
use super::sys::{self, Uname};

/// Default search path, matching agetty's default (see issue.h).
const DEFAULT_PATH: &str = "/etc/issue:/etc/issue.d:/run/issue:/run/issue.d:/usr/lib/issue:/usr/lib/issue.d";

/// Cap total collected issue text to prevent runaway allocation.
const MAX_SIZE: u64 = 256 * 1024;

/// Collects the raw issue text from a colon-separated list of files and
/// directories. Directories are scanned for *.issue files in lexicographic
/// order. A file is ignored if a file with the same name was already read,
/// so /etc/issue takes precedence over /run/issue and /usr/lib/issue.
fn collect(search_path: &str) -> Vec<u8> {
    let mut text = Vec::new();
    let mut read_names: Vec<Vec<u8>> = Vec::new();
    let mut append = |path: &Path, text: &mut Vec<u8>| {
        let name = path.file_name().map_or_else(Vec::new, |n| n.as_encoded_bytes().to_vec());
        if read_names.contains(&name) {
            return;
        }
        let Ok(file) = File::open(path) else {
            return;
        };
        read_names.push(name);
        let limit = MAX_SIZE - text.len() as u64;
        let _ = file.take(limit).read_to_end(text);
    };

    for entry in search_path.split(':').filter(|e| !e.is_empty()) {
        let path = Path::new(entry);
        let Ok(meta) = fs::metadata(path) else {
            continue;
        };
        if meta.is_dir() {
            let Ok(dir) = fs::read_dir(path) else {
                continue;
            };
            let mut files: Vec<_> = dir
                .filter_map(|e| e.ok())
                .map(|e| e.file_name())
                .filter(|n| {
                    let n = n.as_encoded_bytes();
                    !n.starts_with(b".") && n.ends_with(b".issue")
                })
                .collect();
            files.sort();
            for name in files {
                append(&path.join(name), &mut text);
            }
        } else {
            append(path, &mut text);
        }
    }
    text
}

/// Reads a field of os-release(5), removing its double quotes.
fn os_release_field(field: &[u8]) -> Option<Vec<u8>> {
    let content = fs::read("/etc/os-release").or_else(|_| fs::read("/usr/lib/os-release")).ok()?;
    content.split(|&c| c == b'\n').find_map(|line| {
        let value = line.strip_prefix(field)?.strip_prefix(b"=")?;
        let unquoted = value.strip_prefix(b"\"").and_then(|v| v.strip_suffix(b"\""));
        Some(unquoted.unwrap_or(value).to_vec())
    })
}

/// The escape sequence of a color name of the \e{name} escape.
fn color_sequence(name: &[u8]) -> Option<&'static str> {
    Some(match name {
        b"black" => "[30m",
        b"blink" => "[5m",
        b"blue" => "[34m",
        b"bold" => "[1m",
        b"brown" => "[33m",
        b"cyan" => "[36m",
        b"darkgray" => "[1;30m",
        b"gray" | b"lightgray" => "[37m",
        b"green" => "[32m",
        b"halfbright" => "[2m",
        b"lightblue" => "[1;34m",
        b"lightcyan" => "[1;36m",
        b"lightgreen" => "[1;32m",
        b"lightmagenta" => "[1;35m",
        b"lightred" => "[1;31m",
        b"magenta" => "[35m",
        b"red" => "[31m",
        b"reset" => "[0m",
        b"reverse" => "[7m",
        b"white" => "[1;37m",
        b"yellow" => "[1;33m",
        _ => return None,
    })
}

/// Expands the escape sequences of the issue text, and converts \n to \r\n
/// for the terminal.
struct Expander<'a> {
    pty_name: &'a [u8],
    uname: Uname,
    date: String,
    time: String,
    /// Read on the first network escape
    book: Option<AddrBook>,
}

impl Expander<'_> {
    fn expand(&mut self, raw: &[u8]) -> Vec<u8> {
        let mut out = Vec::with_capacity(raw.len());
        let mut pos = 0;
        while let Some(i) = raw[pos..].iter().position(|&c| c == b'\\' || c == b'\n') {
            out.extend_from_slice(&raw[pos..pos + i]);
            pos += i + 1;
            if raw[pos - 1] == b'\n' {
                out.extend_from_slice(b"\r\n");
                continue;
            }
            let Some(&code) = raw.get(pos) else {
                break;
            };
            pos += 1;
            let param = if b"Se46".contains(&code) { parameter(raw, &mut pos) } else { None };
            self.escape(code, param, &mut out);
        }
        if pos < raw.len() {
            out.extend_from_slice(&raw[pos..]);
        }
        out
    }

    fn escape(&mut self, code: u8, param: Option<&[u8]>, out: &mut Vec<u8>) {
        let u = &self.uname;
        let text: &[u8] = match code {
            b'\\' => b"\\",
            b's' => u.sysname.as_bytes(),
            b'n' => u.nodename.as_bytes(),
            b'r' => u.release.as_bytes(),
            b'v' => u.version.as_bytes(),
            b'm' => u.machine.as_bytes(),
            b'o' if u.domainname.is_empty() => b"(none)",
            b'o' => u.domainname.as_bytes(),
            b'd' => self.date.as_bytes(),
            b't' => self.time.as_bytes(),
            b'l' => self.pty_name,
            b'S' => {
                let field = os_release_field(param.unwrap_or(b"PRETTY_NAME"));
                out.extend_from_slice(&field.unwrap_or_default());
                return;
            }
            b'e' => {
                out.push(0x1b);
                if let Some(seq) = param.and_then(color_sequence) {
                    out.extend_from_slice(seq.as_bytes());
                }
                return;
            }
            b'4' | b'6' => {
                let iface = String::from_utf8_lossy(param.unwrap_or_default());
                let book = self.book.get_or_insert_with(AddrBook::new);
                if let Some(ip) = book.best_ip(&iface, code == b'6') {
                    out.extend_from_slice(ip.to_string().as_bytes());
                }
                return;
            }
            b'a' | b'A' => {
                let book = self.book.get_or_insert_with(AddrBook::new);
                out.extend_from_slice(book.all_ip(code == b'a').as_bytes());
                return;
            }
            _ => b"",
        };
        out.extend_from_slice(text);
    }
}

/// Parses the optional {parameter} of an escape at `pos`. An unterminated
/// parameter extends to the end of the text.
fn parameter<'a>(raw: &'a [u8], pos: &mut usize) -> Option<&'a [u8]> {
    let rest = raw[*pos..].strip_prefix(b"{")?;
    let len = rest.iter().position(|&c| c == b'}').unwrap_or(rest.len());
    *pos = (*pos + 1 + len + 1).min(raw.len());
    Some(&rest[..len])
}

/// Returns the expanded issue text, or None if there is none.
pub fn get_buffer(search_path: Option<&str>, pty_name: &[u8]) -> Option<Vec<u8>> {
    let raw = collect(search_path.unwrap_or(DEFAULT_PATH));
    if raw.is_empty() {
        return None;
    }
    let [date, time] = sys::local_time([c"%Y-%m-%d", c"%H:%M:%S"]);
    let mut expander = Expander { pty_name, uname: sys::get_uname(), date, time, book: None };
    Some(expander.expand(&raw))
}

/// Returns a malloc'd, NUL-terminated, buffer containing the expanded issue
/// text, with `pty_name` for \l, and its length in `len`. The caller frees
/// it. Returns NULL if there is no issue text.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kmscon_issue_get_buffer(
    search_path: *const c_char,
    pty_name: *const c_char,
    len: *mut usize,
) -> *mut c_char {
    let to_bytes = |s: *const c_char| (!s.is_null()).then(|| unsafe { CStr::from_ptr(s) }.to_bytes());
    let search_path = to_bytes(search_path).map(String::from_utf8_lossy);
    let pty_name = to_bytes(pty_name).unwrap_or_default();

    let Some(text) = get_buffer(search_path.as_deref(), pty_name) else {
        return ptr::null_mut();
    };
    let buf = unsafe { sys::malloc(text.len() + 1) }.cast::<u8>();
    if buf.is_null() {
        return ptr::null_mut();
    }
    unsafe {
        let out = slice::from_raw_parts_mut(buf, text.len() + 1);
        out[..text.len()].copy_from_slice(&text);
        out[text.len()] = 0;
        *len = text.len();
    }
    buf.cast()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn expand(raw: &[u8]) -> Vec<u8> {
        let uname = Uname {
            sysname: "Linux".into(),
            nodename: "host".into(),
            release: "6.0".into(),
            version: "#1".into(),
            machine: "x86_64".into(),
            domainname: String::new(),
        };
        let mut e = Expander {
            pty_name: b"tty1",
            uname,
            date: "2026-11-18".into(),
            time: "12:00:00".into(),
            book: Some(AddrBook::default()),
        };
        e.expand(raw)
    }

    #[test]
    fn escapes() {
        assert_eq!(expand(b"\\s \\n \\r \\v \\m \\o \\l"), b"Linux host 6.0 #1 x86_64 (none) tty1");
        assert_eq!(expand(b"\\d \\t"), b"2026-11-18 12:00:00");
        assert_eq!(expand(b"a\\\\b\\zc\\"), b"a\\bc");
        assert_eq!(expand(b"a\nb\n"), b"a\r\nb\r\n");
        assert_eq!(expand(b"\\4{nope}\\6\\a\\A\n"), b"\r\n");
        assert_eq!(expand(b"\\s{x}"), b"Linux{x}");
    }

    #[test]
    fn colors() {
        assert_eq!(expand(b"\\e{red}R\\e{reset}"), b"\x1b[31mR\x1b[0m");
        assert_eq!(expand(b"\\e{color-name-that-exceeds-buffer}suffix\n"), b"\x1bsuffix\r\n");
        assert_eq!(expand(b"\\e[1m"), b"\x1b[1m");
        assert_eq!(expand(b"\\e{red"), b"\x1b[31m");
    }

    struct TempDir(std::path::PathBuf);

    impl TempDir {
        fn new(name: &str) -> Self {
            let dir = std::env::temp_dir().join(format!("kmscon-test-{name}-{}", std::process::id()));
            let _ = fs::remove_dir_all(&dir);
            fs::create_dir_all(&dir).unwrap();
            Self(dir)
        }

        fn write(&self, name: &str, content: &str) -> String {
            let path = self.0.join(name);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(&path, content).unwrap();
            path.to_str().unwrap().into()
        }
    }

    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn search_path() {
        let tmp = TempDir::new("issue");
        let etc = tmp.write("etc/issue", "etc\n");
        tmp.write("etc/issue.d/b.issue", "b\n");
        tmp.write("etc/issue.d/a.issue", "a\n");
        tmp.write("etc/issue.d/.hidden.issue", "hidden\n");
        tmp.write("etc/issue.d/c.txt", "txt\n");
        tmp.write("run/issue.d/a.issue", "masked\n");
        tmp.write("run/issue.d/c.issue", "c\n");
        tmp.write("run/issue", "masked\n");
        let root = tmp.0.to_str().unwrap();
        let path = format!("{etc}:{root}/etc/issue.d::{root}/missing:{root}/run/issue.d:{root}/run/issue");
        assert_eq!(collect(&path), b"etc\na\nb\nc\n");
        assert_eq!(collect(""), b"");
        assert_eq!(get_buffer(Some(""), b"tty1"), None);
    }
}
