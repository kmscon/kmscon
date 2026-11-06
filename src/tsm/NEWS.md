# libtsm Release News

## CHANGES WITH 5.0.0
### Rewrite in Rust
libtsm is now a Rust library crate (`tsm`) with a Rust API. The C library,
its headers and the meson build are removed. Escape sequences are parsed with
the `vte` crate and character widths come from the `unicode-width` crate.

### Behavior changes
* CSI e (VPR), CSI G (CHA) and the cursor position report are relative to the
  scroll region in origin mode. Cursors saved by DECSC and modes 1048/1049 are
  restored at their absolute position, after the saved origin mode.
* CSI b (REP) without parameter repeats the character once.
* Escape sequences with unknown intermediates or private markers are ignored
  (ESC # 8, CSI = c, CSI < 1 m, CSI ? 4 m).
* DECSCA (CSI Ps " q) is implemented and protects cells from selective erase;
  it no longer changes the keyboard LEDs.
* Invalid UTF-8 is replaced with U+FFFD; 8-bit mode accepts high bytes.
* Empty CSI parameters are treated as 0.
* DECSCNM no longer inverts cells that already have the inverse attribute.
* Soft and hard resets disable bracketed paste.
* The scroll-back position stays consistent when the scroll-back is shrunk.
* CSI p without `"` is no longer treated as DECSCL, and CSI > p (XTSMPOINTER)
  no longer performs a soft reset.
* Combining characters and other zero-width characters are still dropped.

## CHANGES WITH 4.7.1
### Bug fixes
* libtsm: fix tsm_screen_attr2_t by @kdj0c in https://github.com/kmscon/libtsm/pull/64
* Fix 3 out of bound reads by @kdj0c in https://github.com/kmscon/libtsm/pull/65

## CHANGES WITH 4.7.0
### New cursor interface:
A new tsm_screen_get_cursor_style() interface is available, that provides the current
style of the cursor.

### New features
* screen/vte: add cursor style support via DECSCUSR (CSI Ps SP q) by @underscoreevelyn in https://github.com/kmscon/libtsm/pull/58

### Bug Fixes
* Fix backward selection by @kdj0c in https://github.com/kmscon/libtsm/pull/54
* Fix also clear the character value for multi-width character. by @kdj0c in https://github.com/kmscon/libtsm/pull/56
* screen: don't clear cells when resizing by @kdj0c in https://github.com/kmscon/libtsm/pull/61

## New Contributors
* @underscoreevelyn made their first contribution in https://github.com/kmscon/libtsm/pull/58

## CHANGES WITH 4.6.0
### New draw2 interface
A new tsm_screen_draw2() interface is available that provides a simplified drawing API.
In a future release, we may remove the existing tsm_screen_draw() interface, so it is 
recommended to use tsm_screen_draw2() instead.

### New features
* Add a new draw2 interface by @kdj0c in https://github.com/kmscon/libtsm/pull/50
* vte: Add dim support by @kdj0c in https://github.com/kmscon/libtsm/pull/51
### Bug fixes
* vte: map mouse wheel up/down buttons correctly by @mierenhoop in https://github.com/kmscon/libtsm/pull/45
* fix CSI default parameter by @kdj0c in https://github.com/kmscon/libtsm/pull/47
* vte: Fix ctrl+shift+arrows by @kdj0c in https://github.com/kmscon/libtsm/pull/49

## New Contributors
* @mierenhoop made their first contribution in https://github.com/kmscon/libtsm/pull/45

## CHANGES WITH 4.5.0
### New features
* Support for terminal bell and keyboard LEDs by @aruiz in https://github.com/kmscon/libtsm/pull/34
### Bug fixes
* Fix build musl by @kdj0c in https://github.com/kmscon/libtsm/pull/32
* test/vte: fix memory leak in vte tests by @kdj0c in https://github.com/kmscon/libtsm/pull/35
* Refactor scrollback and selection by @kdj0c in https://github.com/kmscon/libtsm/pull/36
* screen: Fix wrong attribute for new cells when resizing by @kdj0c in https://github.com/kmscon/libtsm/pull/37
* Fix remove from sb by @kdj0c in https://github.com/kmscon/libtsm/pull/38
* Fix get next line by @kdj0c in https://github.com/kmscon/libtsm/pull/39
* Fix scrollback position by @kdj0c in https://github.com/kmscon/libtsm/pull/40
* test: robustness, make the test faster. by @kdj0c in https://github.com/kmscon/libtsm/pull/41

### New Contributors
* @aruiz made their first contribution in https://github.com/kmscon/libtsm/pull/34

## CHANGES WITH 4.4.3
### New features
* Add support for VT200 mouse tracking by @caramelli in https://github.com/kmscon/libtsm/pull/27
### Bug Fixes
* Fix Ctrl-PgUp and Ctrl-PgDn combinations by @Karlson2k in https://github.com/kmscon/libtsm/pull/24
* Fix scrollback reset position in tsm_screen_sb_reset() by @caramelli in https://github.com/kmscon/libtsm/pull/26
* vte: guard case 'm' (SGR) against CSI_GT prefix by @kdj0c in https://github.com/kmscon/libtsm/pull/29
* wcwidth: update to upstream v4 by @kdj0c in https://github.com/kmscon/libtsm/pull/28

### New Contributors
* @Karlson2k made their first contribution in https://github.com/kmscon/libtsm/pull/24
* @caramelli made their first contribution in https://github.com/kmscon/libtsm/pull/26

## CHANGES WITH 4.4.2
### New features
* add nord color scheme by @kdj0c in https://github.com/kmscon/libtsm/pull/21
* meson: include tsm's deps in libtsm_dep by @1ace in https://github.com/kmscon/libtsm/pull/19
### Bug Fixes
* Fix CSI 18t and 19t reporting size. by @kdj0c in https://github.com/kmscon/libtsm/pull/20
* resize: Fix a corner case when resizing by @kdj0c in https://github.com/kmscon/libtsm/pull/22

### New Contributors
* @1ace made their first contribution in https://github.com/kmscon/libtsm/pull/19

## CHANGES WITH 4.4.1
### New features
* Add support to CSI b sequence (repeat last char) by @kdj0c in https://github.com/kmscon/libtsm/pull/15
### Bug fixes
* tsm_vte_paste: check input by @kdj0c in https://github.com/kmscon/libtsm/pull/16

## CHANGES WITH 4.4.0:
### New features
* Add support for CSI 18t and 19t command
* Add bracketed paste support (DEC 2004)

### Bug fixes
 * Moved back to https://github.com/kmscon/libtsm
 * Fix SGR and PIXEL mouse modes blocked by mouse_event check
 * Fix SGR mouse drag tracking in modes 1002 and 1003
 * Fix PIXEL mode drag tracking: remove hardcoded reply_flags
 * Fix ppc64el build error

## CHANGES WITH 4.3.0:
### New features
* Add OSC 4, 10 and 11 support (only read color, not set it) https://github.com/Aetf/libtsm/pull/55
* Add a new API, tsm_screen_selection_word() to select a word at a given position https://github.com/Aetf/libtsm/pull/54

## CHANGES WITH 4.2.0:
### New features
* Add shift arrow keys by @michael-oberpriller in https://github.com/Aetf/libtsm/pull/46
* Convert to Meson by @kdj0c in https://github.com/Aetf/libtsm/pull/45

### Bug fixes
* Fix github CI, after renaming develop to main by @kdj0c in https://github.com/Aetf/libtsm/pull/47
* Meson: fix version-script by @kdj0c in https://github.com/Aetf/libtsm/pull/49
* Readme.md: Fix build status branch name by @kdj0c in https://github.com/Aetf/libtsm/pull/50
* Fix CI on MacOS by @kdj0c in https://github.com/Aetf/libtsm/pull/52


## CHANGES WITH 4.1.0:
### New features!
 * Make backspace key configurable (#23)
 * Add mouse support (#29)
 * Add getters for scrollback buffer line count and position (#32)
 * Add new VGA palette, same as kernel VT (#38)
 * Add ECMA-48 CSI sequences `\E[nE` and `\E[nF` (#39)

### Bug fixes
 * Rewrite of tsm_screen_selection_copy (#36)
 * Fix a memleak in tsm_screen_unref (#35)
 * Check for nullptr in tsm_vte_get_flags (#31)
 * Fix DECRQM SRM request (#30)
 * Fix build on macOS (#24)
 * Fix wrong background color of new cells after resize (#21)
 * Fix path in pkg-config file
