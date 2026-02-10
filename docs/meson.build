# SPDX-FileCopyrightText: 2022 Aetf <aetf@unlimited-code.works>
#
# SPDX-License-Identifier: MIT

manpages = {
  '1': ['kmscon.1.xml.in' 'kmscon.conf.5.xml.in'],
}

data = configuration_data()
data.set('CONFIG_DIR', prefix / sysconfdir)

foreach section, pages : manpages
  foreach page : pages
    xml = configure_file(input: f'man/@page@', output: '@BASENAME@', configuration: data)
    name = fs.replace_suffix(fs.replace_suffix(page, ''), '')
    custom_target(
      name,
      command: [
        xsltproc,
        '--stringparam', 'man.authors.section.enabled', '0',
        '--stringparam', 'man.copyright.section.enabled', '0',
        '--stringparam', 'funcsynopsis.style', 'ansi',
        '--stringparam', 'man.output.quietly', '1',
        '--nonet',
        '-o', '@OUTPUT@',
        manpages_stylesheet,
        '@INPUT@'
      ],
      input: [
        xml
      ],
      output: name,
      build_by_default: true,
      install: true,
      install_dir: mandir / f'man@section@',
    )
  endforeach
endforeach

