#!/bin/sh

if [ "$#" -eq 0 ]; then
	echo "Usage: $0 argument"
	exit 1
fi

# Store current tty number
kms_tty=
active_tty_file=/sys/class/tty/tty0/active
if [ -f "$active_tty_file" ]; then
	read -r kms_tty < "$active_tty_file"
fi

case "${TERM_PROGRAM}" in
tmux) printf "\033Ptmux;\033\033]setBackground\a\033\\" ;;
*) printf "\033]setBackground\a" ;;
esac

"$@"

# If the current tty has changed, wait until the user switches back.
if [ -n "${kms_tty}" ]; then
	read -r check_kms_tty < "$active_tty_file"
	while [ "${check_kms_tty}" != "${kms_tty}" ]; do
		# use inotifywait if available, fallback to plain sleep
		if command -v inotifywait > /dev/null; then
			inotifywait -qqe modify "$active_tty_file"
		else
			sleep 1
		fi
		read -r check_kms_tty < "$active_tty_file"
	done
fi

case "${TERM_PROGRAM}" in
tmux) printf "\033Ptmux;\033\033]setForeground\a\033\\" ;;
*) printf "\033]setForeground\a" ;;
esac
