#!/bin/sh

if [ "$#" -eq 0 ]; then
    echo "Usage: $0 argument"
    exit 1
fi

# Store current tty number
kms_tty=
if [ -f /sys/class/tty/tty0/active ]; then
    read -r kms_tty < /sys/class/tty/tty0/active
fi

if [ "${TERM_PROGRAM}" != "tmux" ]; then
    printf '%b' "\033]setBackground\a"
else
    printf '%b' "\033Ptmux;\033\033]setBackground\a\033\\"
fi

"$@"

# If the current tty has changed, wait until the user switches back.
if [ -n "${kms_tty}" ]; then
    while { read -r check_kms_tty; [ "${check_kms_tty}" != "${kms_tty}" ]; }; do
        sleep 1
    done
fi

if [ "${TERM_PROGRAM}" != "tmux" ]; then
    printf '%b' "\033]setForeground\a"
else
    printf '%b' "\033Ptmux;\033\033]setForeground\a\033\\"
fi
