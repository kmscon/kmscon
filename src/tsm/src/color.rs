//! Colors and color palettes.

/// A 24-bit RGB color.
#[derive(Copy, Clone, PartialEq, Eq, Hash, Default, Debug)]
pub struct Rgb {
    pub r: u8,
    pub g: u8,
    pub b: u8,
}

impl Rgb {
    pub const fn new(r: u8, g: u8, b: u8) -> Self {
        Self { r, g, b }
    }
}

/// Indices into a [`Palette`].
pub mod index {
    pub const BLACK: u8 = 0;
    pub const RED: u8 = 1;
    pub const GREEN: u8 = 2;
    pub const YELLOW: u8 = 3;
    pub const BLUE: u8 = 4;
    pub const MAGENTA: u8 = 5;
    pub const CYAN: u8 = 6;
    pub const LIGHT_GREY: u8 = 7;
    pub const DARK_GREY: u8 = 8;
    pub const LIGHT_RED: u8 = 9;
    pub const LIGHT_GREEN: u8 = 10;
    pub const LIGHT_YELLOW: u8 = 11;
    pub const LIGHT_BLUE: u8 = 12;
    pub const LIGHT_MAGENTA: u8 = 13;
    pub const LIGHT_CYAN: u8 = 14;
    pub const WHITE: u8 = 15;
    /// Default foreground color.
    pub const FOREGROUND: u8 = 16;
    /// Default background color.
    pub const BACKGROUND: u8 = 17;
}

/// The 16 ANSI colors followed by the default foreground and background.
///
/// Index with the constants of [`index`].
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub struct Palette(pub [Rgb; 18]);

impl Default for Palette {
    fn default() -> Self {
        Self::LEGACY
    }
}

impl Palette {
    /// Names accepted by [`Palette::by_name`].
    pub const NAMES: &'static [&'static str] = &[
        "legacy",
        "vga",
        "nord",
        "solarized",
        "solarized-black",
        "solarized-white",
        "soft-black",
        "base16-dark",
        "base16-light",
    ];

    /// Returns one of the predefined palettes.
    pub fn by_name(name: &str) -> Option<Palette> {
        Some(match name {
            "legacy" => Self::LEGACY,
            "vga" => Self::VGA,
            "nord" => Self::NORD,
            "solarized" => Self::SOLARIZED,
            "solarized-black" => Self::SOLARIZED_BLACK,
            "solarized-white" => Self::SOLARIZED_WHITE,
            "soft-black" => Self::SOFT_BLACK,
            "base16-dark" => Self::BASE16_DARK,
            "base16-light" => Self::BASE16_LIGHT,
            _ => return None,
        })
    }

    /// Default foreground color.
    pub fn foreground(&self) -> Rgb {
        self.0[index::FOREGROUND as usize]
    }

    /// Default background color.
    pub fn background(&self) -> Rgb {
        self.0[index::BACKGROUND as usize]
    }

    /// Resolves a color of the xterm 256 color table. The first 16 entries
    /// come from the palette.
    pub fn lookup256(&self, index: u32) -> Rgb {
        const LEVELS: [u8; 6] = [0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff];
        match index {
            0..16 => self.0[index as usize],
            16..232 => {
                let code = index - 16;
                Rgb::new(LEVELS[(code / 36) as usize], LEVELS[(code / 6 % 6) as usize], LEVELS[(code % 6) as usize])
            }
            232..256 => {
                let l = ((index - 232) * 10 + 8) as u8;
                Rgb::new(l, l, l)
            }
            _ => Rgb::default(),
        }
    }

    pub const LEGACY: Palette = Palette([
        Rgb::new(0x00, 0x00, 0x00),
        Rgb::new(0xcd, 0x00, 0x00),
        Rgb::new(0x00, 0xcd, 0x00),
        Rgb::new(0xcd, 0xcd, 0x00),
        Rgb::new(0x00, 0x00, 0xee),
        Rgb::new(0xcd, 0x00, 0xcd),
        Rgb::new(0x00, 0xcd, 0xcd),
        Rgb::new(0xe5, 0xe5, 0xe5),
        Rgb::new(0x7f, 0x7f, 0x7f),
        Rgb::new(0xff, 0x00, 0x00),
        Rgb::new(0x00, 0xff, 0x00),
        Rgb::new(0xff, 0xff, 0x00),
        Rgb::new(0x5c, 0x5c, 0xff),
        Rgb::new(0xff, 0x00, 0xff),
        Rgb::new(0x00, 0xff, 0xff),
        Rgb::new(0xff, 0xff, 0xff),
        Rgb::new(0xe5, 0xe5, 0xe5),
        Rgb::new(0x00, 0x00, 0x00),
    ]);
    pub const VGA: Palette = Palette([
        Rgb::new(0x00, 0x00, 0x00),
        Rgb::new(0xaa, 0x00, 0x00),
        Rgb::new(0x00, 0xaa, 0x00),
        Rgb::new(0xaa, 0x55, 0x00),
        Rgb::new(0x00, 0x00, 0xaa),
        Rgb::new(0xaa, 0x00, 0xaa),
        Rgb::new(0x00, 0xaa, 0xaa),
        Rgb::new(0xaa, 0xaa, 0xaa),
        Rgb::new(0x55, 0x55, 0x55),
        Rgb::new(0xff, 0x55, 0x55),
        Rgb::new(0x55, 0xff, 0x55),
        Rgb::new(0xff, 0xff, 0x55),
        Rgb::new(0x55, 0x55, 0xff),
        Rgb::new(0xff, 0x55, 0xff),
        Rgb::new(0x55, 0xff, 0xff),
        Rgb::new(0xff, 0xff, 0xff),
        Rgb::new(0xaa, 0xaa, 0xaa),
        Rgb::new(0x00, 0x00, 0x00),
    ]);
    pub const NORD: Palette = Palette([
        Rgb::new(0x3b, 0x42, 0x52),
        Rgb::new(0xbf, 0x61, 0x6a),
        Rgb::new(0xa3, 0xbe, 0x8c),
        Rgb::new(0xeb, 0xcb, 0x8b),
        Rgb::new(0x81, 0xa1, 0xc1),
        Rgb::new(0xb4, 0x8e, 0xad),
        Rgb::new(0x88, 0xc0, 0xd0),
        Rgb::new(0xe5, 0xe9, 0xf0),
        Rgb::new(0x4c, 0x56, 0x6a),
        Rgb::new(0xbf, 0x61, 0x6a),
        Rgb::new(0xa3, 0xbe, 0x8c),
        Rgb::new(0xeb, 0xcb, 0x8b),
        Rgb::new(0x81, 0xa1, 0xc1),
        Rgb::new(0xb4, 0x8e, 0xad),
        Rgb::new(0x8f, 0xbc, 0xbb),
        Rgb::new(0xec, 0xef, 0xf4),
        Rgb::new(0xd8, 0xde, 0xe9),
        Rgb::new(0x2e, 0x34, 0x40),
    ]);
    pub const SOLARIZED: Palette = Palette([
        Rgb::new(0x07, 0x36, 0x42),
        Rgb::new(0xdc, 0x32, 0x2f),
        Rgb::new(0x85, 0x99, 0x00),
        Rgb::new(0xb5, 0x89, 0x00),
        Rgb::new(0x26, 0x8b, 0xd2),
        Rgb::new(0xd3, 0x36, 0x82),
        Rgb::new(0x2a, 0xa1, 0x98),
        Rgb::new(0xee, 0xe8, 0xd5),
        Rgb::new(0x00, 0x2b, 0x36),
        Rgb::new(0xcb, 0x4b, 0x16),
        Rgb::new(0x58, 0x6e, 0x75),
        Rgb::new(0x65, 0x7b, 0x83),
        Rgb::new(0x83, 0x94, 0x96),
        Rgb::new(0x6c, 0x71, 0xc4),
        Rgb::new(0x93, 0xa1, 0xa1),
        Rgb::new(0xfd, 0xf6, 0xe3),
        Rgb::new(0xee, 0xe8, 0xd5),
        Rgb::new(0x07, 0x36, 0x42),
    ]);
    pub const SOLARIZED_BLACK: Palette = Palette([
        Rgb::new(0x00, 0x00, 0x00),
        Rgb::new(0xdc, 0x32, 0x2f),
        Rgb::new(0x85, 0x99, 0x00),
        Rgb::new(0xb5, 0x89, 0x00),
        Rgb::new(0x26, 0x8b, 0xd2),
        Rgb::new(0xd3, 0x36, 0x82),
        Rgb::new(0x2a, 0xa1, 0x98),
        Rgb::new(0xee, 0xe8, 0xd5),
        Rgb::new(0x00, 0x2b, 0x36),
        Rgb::new(0xcb, 0x4b, 0x16),
        Rgb::new(0x58, 0x6e, 0x75),
        Rgb::new(0x65, 0x7b, 0x83),
        Rgb::new(0x83, 0x94, 0x96),
        Rgb::new(0x6c, 0x71, 0xc4),
        Rgb::new(0x93, 0xa1, 0xa1),
        Rgb::new(0xfd, 0xf6, 0xe3),
        Rgb::new(0xee, 0xe8, 0xd5),
        Rgb::new(0x00, 0x00, 0x00),
    ]);
    pub const SOLARIZED_WHITE: Palette = Palette([
        Rgb::new(0x07, 0x36, 0x42),
        Rgb::new(0xdc, 0x32, 0x2f),
        Rgb::new(0x85, 0x99, 0x00),
        Rgb::new(0xb5, 0x89, 0x00),
        Rgb::new(0x26, 0x8b, 0xd2),
        Rgb::new(0xd3, 0x36, 0x82),
        Rgb::new(0x2a, 0xa1, 0x98),
        Rgb::new(0xee, 0xe8, 0xd5),
        Rgb::new(0x00, 0x2b, 0x36),
        Rgb::new(0xcb, 0x4b, 0x16),
        Rgb::new(0x58, 0x6e, 0x75),
        Rgb::new(0x65, 0x7b, 0x83),
        Rgb::new(0x83, 0x94, 0x96),
        Rgb::new(0x6c, 0x71, 0xc4),
        Rgb::new(0x93, 0xa1, 0xa1),
        Rgb::new(0xfd, 0xf6, 0xe3),
        Rgb::new(0x07, 0x36, 0x42),
        Rgb::new(0xee, 0xe8, 0xd5),
    ]);
    pub const SOFT_BLACK: Palette = Palette([
        Rgb::new(0x3f, 0x3f, 0x3f),
        Rgb::new(0x70, 0x50, 0x50),
        Rgb::new(0x60, 0xb4, 0x8a),
        Rgb::new(0xdf, 0xaf, 0x8f),
        Rgb::new(0x9a, 0xb8, 0xd7),
        Rgb::new(0xdc, 0x8c, 0xc3),
        Rgb::new(0x8c, 0xd0, 0xd3),
        Rgb::new(0xff, 0xff, 0xff),
        Rgb::new(0x70, 0x90, 0x80),
        Rgb::new(0xdc, 0xa3, 0xa3),
        Rgb::new(0x72, 0xd5, 0xa3),
        Rgb::new(0xf0, 0xdf, 0xaf),
        Rgb::new(0x94, 0xbf, 0xf3),
        Rgb::new(0xec, 0x93, 0xd3),
        Rgb::new(0x93, 0xe0, 0xe3),
        Rgb::new(0xdc, 0xdc, 0xcc),
        Rgb::new(0xdc, 0xdc, 0xcc),
        Rgb::new(0x2c, 0x2c, 0x2c),
    ]);
    pub const BASE16_DARK: Palette = Palette([
        Rgb::new(0x00, 0x00, 0x00),
        Rgb::new(0xab, 0x46, 0x42),
        Rgb::new(0xa1, 0xb5, 0x6c),
        Rgb::new(0xf7, 0xca, 0x88),
        Rgb::new(0x7c, 0xaf, 0xc2),
        Rgb::new(0xba, 0x8b, 0xaf),
        Rgb::new(0x86, 0xc1, 0xb9),
        Rgb::new(0xaa, 0xaa, 0xaa),
        Rgb::new(0x55, 0x55, 0x55),
        Rgb::new(0xab, 0x46, 0x42),
        Rgb::new(0xa1, 0xb5, 0x6c),
        Rgb::new(0xf7, 0xca, 0x88),
        Rgb::new(0x7c, 0xaf, 0xc2),
        Rgb::new(0xba, 0x8b, 0xaf),
        Rgb::new(0x86, 0xc1, 0xb9),
        Rgb::new(0xff, 0xff, 0xff),
        Rgb::new(0xd8, 0xd8, 0xd8),
        Rgb::new(0x18, 0x18, 0x18),
    ]);
    pub const BASE16_LIGHT: Palette = Palette([
        Rgb::new(0x00, 0x00, 0x00),
        Rgb::new(0xab, 0x46, 0x42),
        Rgb::new(0xa1, 0xb5, 0x6c),
        Rgb::new(0xf7, 0xca, 0x88),
        Rgb::new(0x7c, 0xaf, 0xc2),
        Rgb::new(0xba, 0x8b, 0xaf),
        Rgb::new(0x86, 0xc1, 0xb9),
        Rgb::new(0xaa, 0xaa, 0xaa),
        Rgb::new(0x55, 0x55, 0x55),
        Rgb::new(0xab, 0x46, 0x42),
        Rgb::new(0xa1, 0xb5, 0x6c),
        Rgb::new(0xf7, 0xca, 0x88),
        Rgb::new(0x7c, 0xaf, 0xc2),
        Rgb::new(0xba, 0x8b, 0xaf),
        Rgb::new(0x86, 0xc1, 0xb9),
        Rgb::new(0xff, 0xff, 0xff),
        Rgb::new(0x18, 0x18, 0x18),
        Rgb::new(0xd8, 0xd8, 0xd8),
    ]);
}

/// A color as selected by SGR: either a palette entry or a direct RGB value.
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub enum Color {
    /// Index into the [`Palette`] (0..=17).
    Indexed(u8),
    Rgb(Rgb),
}

impl Color {
    pub(crate) fn resolve(self, palette: &Palette, bold: bool) -> Rgb {
        match self {
            // bold causes light colors
            Color::Indexed(i) if bold && i < 8 => palette.0[i as usize + 8],
            Color::Indexed(i) => palette.0[i as usize],
            Color::Rgb(rgb) => rgb,
        }
    }
}
