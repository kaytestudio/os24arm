# desktop.sh - one OS/2 Command Prompt filling the display: the cage
# kiosk compositor running foot. RUNELF \h\busybox sh /h/desktop.sh
# [layout [variant]], e.g. desktop.sh pt, desktop.sh de nodeadkeys
# (default: us). EXIT in the prompt comes back. (dwl.sh: several windows.)
. /h/env.sh "$@"
exec /h/cage -- /h/foot -c /h/foot.ini > /tmp/desktop.log 2>&1
