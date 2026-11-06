//! Character sets that can be designated into G0..G3.

/// A 94/96 character set mapping GL (0x20..0x7f) or GR (0xa0..0xff).
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub(super) enum Charset {
    /// ASCII.
    UnicodeLower,
    /// Latin-1 supplement (also used for all national sets).
    UnicodeUpper,
    DecSupplementalGraphics,
    /// Line drawing.
    DecSpecialGraphics,
}

impl Charset {
    /// Maps the character at `index` (0..96) of the GL or GR half.
    pub(super) fn map(self, index: u32) -> char {
        let table = match self {
            Charset::UnicodeLower => &UNICODE_LOWER,
            Charset::UnicodeUpper => &UNICODE_UPPER,
            Charset::DecSupplementalGraphics => &DEC_SUPPLEMENTAL_GRAPHICS,
            Charset::DecSpecialGraphics => &DEC_SPECIAL_GRAPHICS,
        };
        table[index as usize]
    }
}

// Tables generated from tsm-vte-charsets.c. Index 0 is 0x20 (or 0xa0).
static UNICODE_LOWER: [char; 96] = [
    '\u{0}', '\u{21}', '\u{22}', '\u{23}', '\u{24}', '\u{25}', '\u{26}', '\u{27}', '\u{28}', '\u{29}', '\u{2a}',
    '\u{2b}', '\u{2c}', '\u{2d}', '\u{2e}', '\u{2f}', '\u{30}', '\u{31}', '\u{32}', '\u{33}', '\u{34}', '\u{35}',
    '\u{36}', '\u{37}', '\u{38}', '\u{39}', '\u{3a}', '\u{3b}', '\u{3c}', '\u{3d}', '\u{3e}', '\u{3f}', '\u{40}',
    '\u{41}', '\u{42}', '\u{43}', '\u{44}', '\u{45}', '\u{46}', '\u{47}', '\u{48}', '\u{49}', '\u{4a}', '\u{4b}',
    '\u{4c}', '\u{4d}', '\u{4e}', '\u{4f}', '\u{50}', '\u{51}', '\u{52}', '\u{53}', '\u{54}', '\u{55}', '\u{56}',
    '\u{57}', '\u{58}', '\u{59}', '\u{5a}', '\u{5b}', '\u{5c}', '\u{5d}', '\u{5e}', '\u{5f}', '\u{60}', '\u{61}',
    '\u{62}', '\u{63}', '\u{64}', '\u{65}', '\u{66}', '\u{67}', '\u{68}', '\u{69}', '\u{6a}', '\u{6b}', '\u{6c}',
    '\u{6d}', '\u{6e}', '\u{6f}', '\u{70}', '\u{71}', '\u{72}', '\u{73}', '\u{74}', '\u{75}', '\u{76}', '\u{77}',
    '\u{78}', '\u{79}', '\u{7a}', '\u{7b}', '\u{7c}', '\u{7d}', '\u{7e}', '\u{0}',
];
static UNICODE_UPPER: [char; 96] = [
    '\u{0}', '\u{a1}', '\u{a2}', '\u{a3}', '\u{a4}', '\u{a5}', '\u{a6}', '\u{a7}', '\u{a8}', '\u{a9}', '\u{aa}',
    '\u{ab}', '\u{ac}', '\u{ad}', '\u{ae}', '\u{af}', '\u{b0}', '\u{b1}', '\u{b2}', '\u{b3}', '\u{b4}', '\u{b5}',
    '\u{b6}', '\u{b7}', '\u{b8}', '\u{b9}', '\u{ba}', '\u{bb}', '\u{bc}', '\u{bd}', '\u{be}', '\u{bf}', '\u{c0}',
    '\u{c1}', '\u{c2}', '\u{c3}', '\u{c4}', '\u{c5}', '\u{c6}', '\u{c7}', '\u{c8}', '\u{c9}', '\u{ca}', '\u{cb}',
    '\u{cc}', '\u{cd}', '\u{ce}', '\u{cf}', '\u{d0}', '\u{d1}', '\u{d2}', '\u{d3}', '\u{d4}', '\u{d5}', '\u{d6}',
    '\u{d7}', '\u{d8}', '\u{d9}', '\u{da}', '\u{db}', '\u{dc}', '\u{dd}', '\u{de}', '\u{df}', '\u{e0}', '\u{e1}',
    '\u{e2}', '\u{e3}', '\u{e4}', '\u{e5}', '\u{e6}', '\u{e7}', '\u{e8}', '\u{e9}', '\u{ea}', '\u{eb}', '\u{ec}',
    '\u{ed}', '\u{ee}', '\u{ef}', '\u{f0}', '\u{f1}', '\u{f2}', '\u{f3}', '\u{f4}', '\u{f5}', '\u{f6}', '\u{f7}',
    '\u{f8}', '\u{f9}', '\u{fa}', '\u{fb}', '\u{fc}', '\u{fd}', '\u{fe}', '\u{0}',
];
static DEC_SUPPLEMENTAL_GRAPHICS: [char; 96] = [
    '\u{0}', '\u{a1}', '\u{a2}', '\u{a3}', '\u{0}', '\u{a5}', '\u{0}', '\u{a7}', '\u{a4}', '\u{a9}', '\u{aa}',
    '\u{ab}', '\u{0}', '\u{0}', '\u{0}', '\u{0}', '\u{b0}', '\u{b1}', '\u{b2}', '\u{b3}', '\u{0}', '\u{b5}', '\u{b6}',
    '\u{b7}', '\u{0}', '\u{b9}', '\u{ba}', '\u{bb}', '\u{bc}', '\u{bd}', '\u{0}', '\u{bf}', '\u{c0}', '\u{c1}',
    '\u{c2}', '\u{c3}', '\u{c4}', '\u{c5}', '\u{c6}', '\u{c7}', '\u{c8}', '\u{c9}', '\u{ca}', '\u{cb}', '\u{cc}',
    '\u{cd}', '\u{ce}', '\u{cf}', '\u{0}', '\u{d1}', '\u{d2}', '\u{d3}', '\u{d4}', '\u{d5}', '\u{d6}', '\u{152}',
    '\u{d8}', '\u{d9}', '\u{da}', '\u{db}', '\u{dc}', '\u{178}', '\u{0}', '\u{df}', '\u{e0}', '\u{e1}', '\u{e2}',
    '\u{e3}', '\u{e4}', '\u{e5}', '\u{e6}', '\u{e7}', '\u{e8}', '\u{e9}', '\u{ea}', '\u{eb}', '\u{ec}', '\u{ed}',
    '\u{ee}', '\u{ef}', '\u{0}', '\u{f1}', '\u{f2}', '\u{f3}', '\u{f4}', '\u{f5}', '\u{f6}', '\u{153}', '\u{f8}',
    '\u{f9}', '\u{fa}', '\u{fb}', '\u{fc}', '\u{ff}', '\u{0}', '\u{0}',
];
static DEC_SPECIAL_GRAPHICS: [char; 96] = [
    '\u{0}', '\u{21}', '\u{22}', '\u{23}', '\u{24}', '\u{25}', '\u{26}', '\u{27}', '\u{28}', '\u{29}', '\u{2a}',
    '\u{2b}', '\u{2c}', '\u{2d}', '\u{2e}', '\u{2f}', '\u{30}', '\u{31}', '\u{32}', '\u{33}', '\u{34}', '\u{35}',
    '\u{36}', '\u{37}', '\u{38}', '\u{39}', '\u{3a}', '\u{3b}', '\u{3c}', '\u{3d}', '\u{3e}', '\u{3f}', '\u{40}',
    '\u{41}', '\u{42}', '\u{43}', '\u{44}', '\u{45}', '\u{46}', '\u{47}', '\u{48}', '\u{49}', '\u{4a}', '\u{4b}',
    '\u{4c}', '\u{4d}', '\u{4e}', '\u{4f}', '\u{50}', '\u{51}', '\u{52}', '\u{53}', '\u{54}', '\u{55}', '\u{56}',
    '\u{57}', '\u{58}', '\u{59}', '\u{5a}', '\u{5b}', '\u{5c}', '\u{5d}', '\u{5e}', '\u{0}', '\u{2666}', '\u{2592}',
    '\u{2409}', '\u{240c}', '\u{240d}', '\u{240a}', '\u{b0}', '\u{b1}', '\u{2424}', '\u{240b}', '\u{2518}', '\u{2510}',
    '\u{250c}', '\u{2514}', '\u{253c}', '\u{23ba}', '\u{23bb}', '\u{2500}', '\u{23bc}', '\u{23bd}', '\u{251c}',
    '\u{2524}', '\u{2534}', '\u{252c}', '\u{2502}', '\u{2264}', '\u{2265}', '\u{3c0}', '\u{2260}', '\u{a3}',
    '\u{22c5}', '\u{0}',
];
