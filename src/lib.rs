// SPDX-License-Identifier: MIT
//
// kmscon-rs: the parts of kmscon written in Rust, and their C bindings.
//
// This crate is built as one static library, linked into the kmscon binary.
// All the Rust code must go through it: linking two Rust static libraries
// into the same binary would duplicate the Rust standard library.

pub mod tsm;
