# desktop.sh - one OS/2 Command Prompt filling the display: the cage
# kiosk compositor running foot. RUNELF \h\busybox sh /h/OS2/desktop.sh
# [layout [variant]], e.g. desktop.sh pt, desktop.sh de nodeadkeys
# (default: us). EXIT in the prompt comes back. (dwl.sh: several windows.)
. /h/OS2/env.sh "$@"
exec /h/OS2/cage -- /h/OS2/foot -c /h/OS2/foot.ini > /tmp/desktop.log 2>&1
