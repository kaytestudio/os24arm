# desktop.sh - a terminal on the display: the cage compositor running the
# foot terminal, with a busybox shell in it. From the OS/2 shell:
#   MOUNT usb0p1 \h
#   RUNELF \h\busybox sh /h/desktop.sh [layout [variant]]
# e.g. desktop.sh pt, desktop.sh de nodeadkeys (default: us).
# (exit the shell in foot to come back.)
export FONTCONFIG_FILE=/h/fonts.conf
# keyboard layouts (xkeyboard-config) are on the stick
export XKB_CONFIG_ROOT=/h/xkb
export XKB_DEFAULT_LAYOUT="${1:-us}"
[ -n "$2" ] && export XKB_DEFAULT_VARIANT="$2"
export XCOMPOSEFILE=/h/Compose          # dead keys and compose sequences
export LANG=C.UTF-8
export XDG_RUNTIME_DIR=/tmp HOME=/tmp SHELL=/h/busybox
export PATH=/h
cd /tmp
exec /h/cage -- /h/foot -c /h/foot.ini /h/busybox sh
