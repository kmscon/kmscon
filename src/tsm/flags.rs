//! A minimal bit-flag newtype generator, to avoid depending on `bitflags`.

/// Declares a `Copy` newtype over an unsigned integer with named flag
/// constants and the usual set operations.
macro_rules! flags {
    (
        $(#[$meta:meta])*
        $vis:vis struct $name:ident: $ty:ty {
            $(
                $(#[$fmeta:meta])*
                const $flag:ident = $value:expr;
            )*
        }
    ) => {
        $(#[$meta])*
        #[derive(Copy, Clone, PartialEq, Eq, Hash, Default)]
        $vis struct $name($ty);

        #[allow(dead_code)]
        impl $name {
            $(
                $(#[$fmeta])*
                pub const $flag: Self = Self($value);
            )*

            /// All named flags, with their names, in bit order.
            pub const NAMED: &'static [(&'static str, Self)] = &[$((stringify!($flag), Self::$flag)),*];

            /// No flag set.
            pub const fn empty() -> Self {
                Self(0)
            }

            /// Raw bit value.
            pub const fn bits(self) -> $ty {
                self.0
            }

            /// Builds a value from raw bits, keeping unknown bits.
            pub const fn from_bits_retain(bits: $ty) -> Self {
                Self(bits)
            }

            /// Returns `true` if no flag is set.
            pub const fn is_empty(self) -> bool {
                self.0 == 0
            }

            /// Returns `true` if all flags of `other` are set.
            pub const fn contains(self, other: Self) -> bool {
                self.0 & other.0 == other.0
            }

            /// Returns `true` if any flag of `other` is set.
            pub const fn intersects(self, other: Self) -> bool {
                self.0 & other.0 != 0
            }

            /// Sets the flags of `other`.
            pub fn insert(&mut self, other: Self) {
                self.0 |= other.0;
            }

            /// Clears the flags of `other`.
            pub fn remove(&mut self, other: Self) {
                self.0 &= !other.0;
            }

            /// Sets or clears the flags of `other`.
            pub fn set(&mut self, other: Self, value: bool) {
                if value {
                    self.insert(other);
                } else {
                    self.remove(other);
                }
            }
        }

        impl ::core::ops::BitOr for $name {
            type Output = Self;
            fn bitor(self, rhs: Self) -> Self {
                Self(self.0 | rhs.0)
            }
        }

        impl ::core::ops::BitOrAssign for $name {
            fn bitor_assign(&mut self, rhs: Self) {
                self.0 |= rhs.0;
            }
        }

        impl ::core::ops::BitAnd for $name {
            type Output = Self;
            fn bitand(self, rhs: Self) -> Self {
                Self(self.0 & rhs.0)
            }
        }

        impl ::core::ops::Not for $name {
            type Output = Self;
            fn not(self) -> Self {
                Self(!self.0)
            }
        }

        impl ::core::fmt::Debug for $name {
            fn fmt(&self, f: &mut ::core::fmt::Formatter<'_>) -> ::core::fmt::Result {
                let mut rest = self.0;
                let mut first = true;
                write!(f, "{}(", stringify!($name))?;
                for (name, flag) in Self::NAMED {
                    if self.contains(*flag) && flag.0 != 0 {
                        write!(f, "{}{}", if first { "" } else { " | " }, name)?;
                        rest &= !flag.0;
                        first = false;
                    }
                }
                if rest != 0 {
                    write!(f, "{}{:#x}", if first { "" } else { " | " }, rest)?;
                }
                write!(f, ")")
            }
        }
    };
}

pub(crate) use flags;
