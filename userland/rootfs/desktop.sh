# desktop.sh - a terminal on the display: the cage compositor running the
# foot terminal, with a busybox shell in it. From the OS/2 shell:
#   MOUNT usb0p1 \h
#   RUNELF \h\busybox sh /h/desktop.sh
# (exit the shell in foot to come back.)
export FONTCONFIG_FILE=/h/fonts.conf
# xkbcommon refuses to create a context when its data directory is missing
# (no xkeyboard-config installed); clients only need the context - their
# keymap comes from the compositor.
export XKB_CONFIG_ROOT=/tmp
export LANG=C.UTF-8
export XDG_RUNTIME_DIR=/tmp HOME=/tmp SHELL=/h/busybox
export PATH=/h
cd /tmp
exec /h/cage -- /h/foot -c /h/foot.ini /h/busybox sh
