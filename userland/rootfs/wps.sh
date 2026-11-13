# wps.sh - the desktop: the Workplace Shell (userland/wps), an OS/2 Warp
# style Wayland compositor. Started by the shell's DESKTOP command
# (kernel/src/main.c), which mounts the stick at \h first; by hand:
# RUNELF \h\busybox sh /h/OS2/wps.sh [layout [variant]].
# Double-click the desktop's objects; the WarpCenter (top) has the system
# menu, the Window List, a Command Prompt, the drives and the programs.
# Keys: Ctrl+Esc Window List, Alt+Tab next window, Alt+F4 close, Alt+O or
# Alt+Shift+Enter Command Prompt, Alt+P programs, Alt+Shift+Q quit.
# What the compositor and its programs print goes to /tmp/desktop.log.
. /h/OS2/env.sh "$@"
exec /h/OS2/wps > /tmp/desktop.log 2>&1
