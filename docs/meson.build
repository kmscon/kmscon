# SPDX-FileCopyrightText: 2022 Aetf <aetf@unlimited-code.works>
#
# SPDX-License-Identifier: MIT

manpages = {
    '1': ['kmscon'],
    '5': ['kmscon.conf'],
}

data = configuration_data()
data.set('CONFIG_DIR', prefix / sysconfdir)

foreach section, pages : manpages
    foreach page : pages
        name = f'@page@.@section@'
        xml = configure_file(
            input: f'man/@name@.xml.in',
            output: '@BASENAME@',
            configuration: data,
        )
        custom_target(
            name,
            command: [
                xsltproc,
                '--stringparam',
                'man.authors.section.enabled',
                '0',
                '--stringparam',
                'man.copyright.section.enabled',
                '0',
                '--stringparam',
                'funcsynopsis.style',
                'ansi',
                '--stringparam',
                'man.output.quietly',
                '1',
                '--nonet',
                '-o',
                '@OUTPUT@',
                manpages_stylesheet,
                '@INPUT@',
            ],
            input: [xml],
            output: name,
            build_by_default: true,
            install: true,
            install_dir: mandir / f'man@section@',
        )
    endforeach
endforeach
