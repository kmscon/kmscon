# SPDX-FileCopyrightText: 2022 Aetf <aetf@unlimited-code.works>
#
# SPDX-License-Identifier: MIT

#
# htable - hash table
#
htable = static_library('ext-htable', ['htable.c'])

htable_deps = declare_dependency(
  link_with: [htable],
  include_directories: [
    include_directories('.'),
  ]
)

