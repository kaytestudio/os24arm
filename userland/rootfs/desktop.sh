# desktop.sh - one terminal on the display: the cage kiosk compositor
# running foot, with a busybox shell in it. From the OS/2 shell:
#   MOUNT usb0p1 \h
#   RUNELF \h\busybox sh /h/desktop.sh [layout [variant]]
# e.g. desktop.sh pt, desktop.sh de nodeadkeys (default: us).
# Exit the shell in foot to come back. (dwl.sh: several windows.)
. /h/env.sh "$@"
exec /h/cage -- /h/foot -c /h/foot.ini
