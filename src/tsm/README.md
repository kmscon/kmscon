# TSM - Terminal Emulator State Machine

TSM is a state machine for DEC VT100-VT520 compatible terminal emulators. It
tries to support all common standards while keeping compatibility to existing
emulators like xterm, gnome-terminal, konsole, ..

TSM itself does not provide any rendering nor window management. It is a simple
plain state machine without any I/O. It can be used to implement terminal
emulators, but also to implement other applications that need to interpret
terminal escape sequences. TSM keeps compatibility as close to xterm as
possible, so the TERM variable can be set to xterm-256color with any TSM based
terminal emulator.

Since version 5, TSM is a Rust library. Earlier versions were written in C.

This copy is bundled in kmscon. Besides the Rust API, it provides the C API
used by kmscon (`tsm.h`, implemented in `src/ffi.rs`), and is built by meson
as a static library.

## Usage

```rust
use tsm::{Handler, Keysym, Modifiers, Vte};

struct Pty; // your connection to the application

impl Handler for Pty {
    fn write(&mut self, data: &[u8]) {
        // send replies and keyboard/mouse input to the application
    }
    fn bell(&mut self) {}
}

let mut vte = Vte::new(Pty);
vte.screen_mut().resize(120, 40);
vte.screen_mut().set_max_scrollback(1000);

// feed what the application writes
vte.input(b"\x1b[1;32mhello\x1b[m world\r\n");

// forward key presses
vte.handle_keyboard(Keysym::UP, Keysym::NO_SYMBOL, Modifiers::empty(), None);

// draw the visible cells
for cell in vte.screen().render() {
    // cell.ch, cell.fg, cell.bg, cell.flags
}
```

The main types are:

* `Vte`: parses the application output, owns the `Screen`, and encodes
  keyboard (`handle_keyboard`), mouse (`handle_mouse`) and paste input.
* `Handler`: receives bytes for the application and events (bell, OSC
  strings, keyboard LEDs, mouse mode changes).
* `Screen`: the cell grid with cursor, scroll-back buffer and selection.
* `Palette`: the 16 colors plus default foreground and background, with
  several predefined palettes.

## Build and test

```bash
cargo build
cargo test
```

From kmscon, `meson test` also runs `cargo test`.

Dependencies: [vte](https://crates.io/crates/vte) (escape sequence parser)
and [unicode-width](https://crates.io/crates/unicode-width).

The behavior is checked by golden snapshot tests, see
[tests/golden/README.md](tests/golden/README.md).

## License

This software is licensed under the terms of an MIT-like license. Please see
[COPYING](COPYING) for further information.

## Contact

This software is maintained by:
 * Jocelyn Falempe <jfalempe@redhat.com>

If you have any questions, do not hesitate to contact one of the maintainers.
