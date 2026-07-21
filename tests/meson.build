# SPDX-FileCopyrightText: 2022 Aetf <aetf@unlimited-code.works>
#
# SPDX-License-Identifier: MIT

foreach name, deps : {
  'output': [uterm_deps],
  'vt': [uterm_deps],
  'input': [uterm_deps],
  'key': [],
}
  deps += [
    shl_deps,
    misc_deps,
    xkbcommon_deps,
  ]
  test_name = f'test_@name@'
  exe = executable(test_name, f'@test_name@.c',
    dependencies: deps,
  )
endforeach

test_shl = executable('test_shl', 'test_shl.c',
  dependencies: [shl_deps, check_deps],
)
test('test_shl', test_shl,
  protocol: 'tap',
  env: {'CK_TAP_LOG_FILE_NAME': '-', 'CK_VERBOSITY': 'silent'},
)

test_text = executable('test_text', 'test_text.c',
  dependencies: [libtsm_deps, shl_deps],
)
test('test_text', test_text)

test_bbulk = executable('test_bbulk', 'test_bbulk.c',
  include_directories: src_inc,
  dependencies: [libtsm_deps, htable_deps],
)
test('test_bbulk', test_bbulk)

test_asciinema = executable('test_asciinema',
  'test_asciinema.c',
  '../src/misc/asciinema.c',
  include_directories: misc_inc,
  dependencies: [libtsm_deps, shl_deps, eloop_deps],
)
test('test_asciinema', test_asciinema)

test_issue = executable('test_issue', 'test_issue.c', '../src/misc/issue.c', '../src/misc/issue_network.c',
  include_directories: misc_inc,
  dependencies: [libtsm_deps, shl_deps],
)
test('test_issue', test_issue)

test_issue_parameter = executable('test_issue_parameter', 'test_issue_parameter.c',
  '../src/misc/issue.c', '../src/misc/issue_network.c',
  include_directories: misc_inc,
  dependencies: [libtsm_deps, shl_deps],
)
test('test_issue_parameter', test_issue_parameter)
