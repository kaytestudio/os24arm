# dwl.sh - the desktop: the dwl compositor, opening on an OS/2 Command
# Prompt. Started by the shell's DESKTOP command (kernel/src/main.c), which
# mounts the stick at \h first; by hand: RUNELF \h\busybox sh /h/dwl.sh
# [layout [variant]].
# Keys (Alt is the modifier): Alt+Shift+Enter or Alt+O an OS/2 Command
# Prompt, Alt+P the programs (fuzzel), Alt+J/K focus next/previous,
# Alt+Enter zoom, Alt+H/L resize, Alt+Shift+C close a window, Alt+T/F/M
# tiled/floating/monocle layout, Alt+1..9 tags, Alt+Shift+Q quit.
# What the compositor and terminals print goes to /tmp/desktop.log, not
# the console.
. /h/env.sh "$@"
exec /h/dwl -s "/h/foot -c /h/foot.ini" > /tmp/desktop.log 2>&1
