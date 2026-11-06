# Golden snapshot tests

Each `scenarios/NAME.scn` file is a scenario written in the small language
described below, and `scenarios/NAME.out` is its expected transcript. The
transcripts were first recorded from the C implementation of libtsm 4.x and
are the regression oracle of this crate. Scenarios whose behavior was changed
on purpose during the Rust rewrite start with a `# Note:` comment.

```
cargo test --test golden                         # run all scenarios
GOLDEN_FILTER=sgr cargo test --test golden       # run matching scenarios
GOLDEN_BLESS=1 cargo test --test golden          # rewrite failing .out files
```

After blessing, review the `.out` diff before committing.

## Scenario language

One command per line. Leading/trailing whitespace is ignored, `#` starts a
comment. Arguments are words, `key=value` pairs, or double-quoted strings.
Strings support `\e \a \b \t \n \r \f \v \0 \\ \" \xHH` and `\u{HEX}` (encoded
as UTF-8).

Every scenario starts with a fresh 80x24 screen and a vte that has all
callbacks (write, osc, mouse, bell, led) installed.

| Command | Action |
|---|---|
| `resize COLS ROWS` | resize the screen |
| `in STR...` | feed the bytes to the vte in one call |
| `in1 STR...` | feed the bytes one byte per call |
| `hex HH...` | feed raw bytes in one call |
| `key KEYSYM [ascii=KEYSYM] [mods=M+M] [uni=STR\|U+HHHH\|none]` | keyboard event; prints `< ret` |
| `type STR` | one key event per character (keysym = code point, or `0x01000000\|cp` above U+00FF) |
| `mouse EVENT BUTTON cell=X,Y [px=X,Y] [mods=M+M]` | mouse event; prints `< ret` |
| `paste STR` | paste |
| `sel-start X Y`, `sel-target X Y`, `sel-word X Y`, `sel-reset` | selection |
| `sel-copy` | prints `< copy "..."` or `< copy none` |
| `sb-max N`, `sb-clear`, `sb-up N`, `sb-down N`, `sb-page-up N`, `sb-page-down N`, `sb-reset` | scrollback |
| `palette NAME\|none` | select a named palette (`custom` selects the custom one) |
| `custom-palette RRGGBB x18 \| none` | set the custom palette |
| `bs-delete on\|off` | backspace sends DEL |
| `screen-set FLAG...`, `screen-reset FLAG...` | screen flags (names as in `screen-flags`) |
| `margins TOP BOTTOM` | set scroll margins (1-based, as DECSTBM) |
| `reset`, `hard-reset` | soft / hard vte reset |
| `dump [state] [text] [attrs] [render]` | snapshot, default `state text attrs` |
| `width-table` | run-length table of character widths |

* `KEYSYM`: an xkbcommon name without `XKB_KEY_` (`Up`, `KP_Enter`, `F5`,
  `BackSpace`, `space`, `bracketleft`, ...), a single printable ASCII
  character, or a hexadecimal value `0x...`.
* key `mods`: `shift lock ctrl alt logo`; mouse `mods`: `shift meta ctrl`.
* mouse `EVENT`: `press release move` or a number (bit mask);
  `BUTTON`: `left middle right wheelup wheeldown` or a number.

## Transcript format

* Every command is echoed as `> COMMAND LINE`.
* Side effects follow in the order they happen:
  * `< write "..."` bytes sent to the host; consecutive writes are merged
  * `< osc "..."`, `< bell`, `< led scroll+num+caps|none`,
    `< mouse-cb mode=N pixels=0|1`
  * `< ret true|false`, `< copy ...`, `< width START-END W`
* Byte strings use `\e \a \b \t \n \r \\ \"`, printable ASCII as is and
  `\xhh` for everything else.

### `state`

```
state
  size COLSxROWS cursor X,Y          raw cursor: X may be >= COLS (pending wrap)
  cursor-style N blink MS
  margins TOP-BOTTOM                 0-based, inclusive
  tabs 0,8,...                       or -
  screen-flags A|B                   or -
  vte-flags A|B                      or -
  mouse-mode N
  sb count=N pos=N max=N
  default RRGGBB/RRGGBB              vte default foreground/background
  erase RRGGBB/RRGGBB FLAGS          attributes used for erased cells (BCE)
```

### `text`

The active screen (not the scrollback view), one row per line between `|`.
`·` is an empty cell (code point 0), a wide character is printed once and its
trailing cell is omitted. Characters that cannot be printed literally are shown
as `�`. A `cells` block follows if any cell is unusual (unprintable code point,
a width-0 cell not following a width-2 cell, a width-2 cell without a trailing
cell, ...): `  ROW,COL U+XXXX w=WIDTH`.

### `attrs`

Runs of identical cell attributes that differ from the vte default, per row:
`  ROW: COL[-COL] FG/BG FLAGS`. Flags are `B`old `D`im `I`talic `U`nderline
blin`K` `R`everse `P`rotect, or `-`.

### `render`

The output of the draw interface: the visible view (including scrollback
position, selection highlight, inverse screen and dim blending). A text grid
(empty cells shown as spaces, wide characters followed by a space) then runs
of cells that differ from default fg/bg without flags:
`  ROW: COL[-COL] FG/BG FLAGS` with flags `B I U K` or `-`.
