#
# input
#

keymap_regex = [
  '--regex', '\\s*(=,)\\s*',       '\\1', # Whitespace around assignments and commas
  '--regex', '\\s*([\\]\\[])\\s*', '\\1', # Whitespace around []
  '--regex', '\\s*([{}])\\s*',     '\\1', # Whitespace around {}
  '--regex', '\\s*([()])\\s*',     '\\1', # Whitespace around ()
  '--regex', '^\\s*',              '',    # Indentation whitespace
  '--regex', '[\\r\\n]',           '',    # Newlines
  '--zero_term',                          # append an ASCII 0 so it can be used as regular C-string
]

input_srcs = [
  'input.c',
  'input_uxkb.c',
  'input_pointer.c',
  embed_gen.process('input_fallback.xkb', extra_args: keymap_regex),
]

input_dep = [
  libudev_deps,
  xkbcommon_deps,
  shl_deps,
  eloop_deps,
]

input = static_library('input', input_srcs,
  dependencies: input_dep
)

input_deps = declare_dependency(
  link_with: [input],
  dependencies: input_dep
)