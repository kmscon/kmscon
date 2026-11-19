#!/usr/bin/bash

check() {
    require_binaries kmscon systemd-tty-ask-password-agent || return 1
    return 0
}

depends() {
    # simpledrm should be built in the kernel, and is enough to run kmscon.
    echo systemd
    return 0
}

installkernel() {
    # Ensure keyboard input event driver is present
    hostonly='' instmods evdev
}

install() {
    inst_multiple \
        kmscon \
        systemd-tty-ask-password-agent

    inst_multiple -o \
        /etc/kmscon/kmscon.conf \
        /usr/share/terminfo/x/xterm-256color \
        /usr/share/terminfo/k/kmscon \

    inst_multiple -o \
        "${systemdsystemunitdir}/systemd-ask-password-kmscon.path" \
        "${systemdsystemunitdir}/systemd-ask-password-kmscon.service"

    # Install unifont plugin for non-latin languages
    # and resolve dependencies
    inst_libdir_file "kmscon/mod-unifont.so"

    # Install XKB keymap definitions required by libxkbcommon for keyboard input
    inst_dir /usr/share/X11/xkb
    cp -a /usr/share/X11/xkb/. "${initdir}/usr/share/X11/xkb/"

    # Enable the path watch
    $SYSTEMCTL -q --root "$initdir" enable systemd-ask-password-kmscon.path
}
