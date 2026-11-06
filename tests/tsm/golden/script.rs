//! Scenario language tokenizer, see README.md.

#[derive(Debug, Clone)]
pub struct Token {
    /// For `key=value` tokens.
    pub key: Option<String>,
    /// Decoded bytes for quoted strings, raw text otherwise.
    pub val: Vec<u8>,
    pub quoted: bool,
}

impl Token {
    pub fn text(&self) -> &str {
        std::str::from_utf8(&self.val).expect("token is valid UTF-8")
    }
}

fn hexval(c: u8) -> Option<u32> {
    (c as char).to_digit(16)
}

fn parse_string(s: &[u8], i: &mut usize) -> Result<Vec<u8>, String> {
    let mut out = Vec::new();
    *i += 1;
    loop {
        let Some(&c) = s.get(*i) else {
            return Err("unterminated string".into());
        };
        *i += 1;
        match c {
            b'"' => return Ok(out),
            b'\\' => {
                let e = *s.get(*i).ok_or("bad escape")?;
                *i += 1;
                match e {
                    b'e' => out.push(0x1b),
                    b'a' => out.push(0x07),
                    b'b' => out.push(0x08),
                    b't' => out.push(0x09),
                    b'n' => out.push(0x0a),
                    b'r' => out.push(0x0d),
                    b'f' => out.push(0x0c),
                    b'v' => out.push(0x0b),
                    b'0' => out.push(0x00),
                    b'\\' => out.push(b'\\'),
                    b'"' => out.push(b'"'),
                    b'x' => {
                        let h1 = s.get(*i).copied().and_then(hexval).ok_or("bad \\x escape")?;
                        let h2 = s.get(*i + 1).copied().and_then(hexval).ok_or("bad \\x escape")?;
                        out.push((h1 * 16 + h2) as u8);
                        *i += 2;
                    }
                    b'u' => {
                        if s.get(*i) != Some(&b'{') {
                            return Err("bad \\u escape".into());
                        }
                        *i += 1;
                        let mut c: u32 = 0;
                        while let Some(h) = s.get(*i).copied().and_then(hexval) {
                            c = c * 16 + h;
                            *i += 1;
                        }
                        if s.get(*i) != Some(&b'}') || c > 0x10ffff {
                            return Err("bad \\u escape".into());
                        }
                        *i += 1;
                        push_utf8(&mut out, c);
                    }
                    _ => return Err(format!("unknown escape \\{}", e as char)),
                }
            }
            _ => out.push(c),
        }
    }
}

/// Encodes any value up to 0x10FFFF, surrogates included.
fn push_utf8(out: &mut Vec<u8>, c: u32) {
    if c < 0x80 {
        out.push(c as u8);
    } else if c < 0x800 {
        out.extend([0xc0 | (c >> 6) as u8, 0x80 | (c & 0x3f) as u8]);
    } else if c < 0x10000 {
        out.extend([0xe0 | (c >> 12) as u8, 0x80 | ((c >> 6) & 0x3f) as u8, 0x80 | (c & 0x3f) as u8]);
    } else {
        out.extend([
            0xf0 | (c >> 18) as u8,
            0x80 | ((c >> 12) & 0x3f) as u8,
            0x80 | ((c >> 6) & 0x3f) as u8,
            0x80 | (c & 0x3f) as u8,
        ]);
    }
}

fn is_space(c: u8) -> bool {
    c == b' ' || c == b'\t'
}

pub fn tokenize(line: &str) -> Result<Vec<Token>, String> {
    let s = line.as_bytes();
    let mut toks = Vec::new();
    let mut i = 0;
    loop {
        while i < s.len() && is_space(s[i]) {
            i += 1;
        }
        if i >= s.len() || s[i] == b'#' {
            break;
        }
        let tok = if s[i] == b'"' {
            Token { key: None, val: parse_string(s, &mut i)?, quoted: true }
        } else {
            let start = i;
            while i < s.len() && !is_space(s[i]) && !matches!(s[i], b'=' | b'"' | b'#') {
                i += 1;
            }
            if s.get(i) == Some(&b'=') {
                let key = String::from_utf8(s[start..i].to_vec()).unwrap();
                i += 1;
                if s.get(i) == Some(&b'"') {
                    Token { key: Some(key), val: parse_string(s, &mut i)?, quoted: true }
                } else {
                    let vstart = i;
                    while i < s.len() && !is_space(s[i]) && s[i] != b'#' {
                        i += 1;
                    }
                    Token { key: Some(key), val: s[vstart..i].to_vec(), quoted: false }
                }
            } else {
                Token { key: None, val: s[start..i].to_vec(), quoted: false }
            }
        };
        if i < s.len() && !is_space(s[i]) && s[i] != b'#' {
            return Err(format!("unexpected character '{}'", s[i] as char));
        }
        toks.push(tok);
    }
    Ok(toks)
}

/// Decodes the first code point of a UTF-8 string, returning it and its
/// encoded length.
pub fn first_codepoint(s: &[u8]) -> Result<(u32, usize), String> {
    let n = match s.first() {
        None => return Err("empty string".into()),
        Some(b) if *b < 0x80 => return Ok((*b as u32, 1)),
        Some(b) if b & 0xe0 == 0xc0 => 2,
        Some(b) if b & 0xf0 == 0xe0 => 3,
        Some(b) if b & 0xf8 == 0xf0 => 4,
        _ => return Err("invalid UTF-8 in string".into()),
    };
    if s.len() < n {
        return Err("truncated UTF-8 in string".into());
    }
    let mut c = (s[0] & (0x7f >> n)) as u32;
    for &b in &s[1..n] {
        if b & 0xc0 != 0x80 {
            return Err("invalid UTF-8 in string".into());
        }
        c = (c << 6) | (b & 0x3f) as u32;
    }
    Ok((c, n))
}
