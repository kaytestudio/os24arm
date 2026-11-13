# dwl.sh - a tiling desktop: the dwl compositor, starting with one foot
# terminal. From the OS/2 shell:
#   MOUNT usb0p1 \h
#   RUNELF \h\busybox sh /h/dwl.sh [layout [variant]]
# Keys (Alt is the modifier): Alt+Shift+Enter a new terminal, Alt+J/K
# focus next/previous, Alt+Enter zoom, Alt+H/L resize, Alt+Shift+C close a
# window, Alt+T/F/M tiled/floating/monocle layout, Alt+1..9 tags,
# Alt+Shift+Q quit.
. /h/env.sh "$@"
exec /h/dwl -s "/h/foot -c /h/foot.ini"
