# env.sh - the environment for the Wayland session (sourced by desktop.sh
# and dwl.sh): fonts, keyboard layout ($1, variant $2; default us),
# compose sequences, paths. Everything lives on the stick in \h\OS2, a
# hidden system folder (userland/make-stick.sh).
export FONTCONFIG_FILE=/h/OS2/fonts.conf
export XKB_CONFIG_ROOT=/h/OS2/xkb
export XKB_DEFAULT_LAYOUT="${1:-us}"
[ -n "$2" ] && export XKB_DEFAULT_VARIANT="$2"
export XCOMPOSEFILE=/h/OS2/Compose          # dead keys and compose sequences
export LANG=C.UTF-8
export XDG_RUNTIME_DIR=/tmp HOME=/tmp SHELL=/h/OS2/busybox
export XDG_DATA_DIRS=/h/OS2 XDG_DATA_HOME=/tmp XDG_CONFIG_HOME=/tmp   # /h/OS2/applications
export PATH=/usr/bin:/bin:/h/OS2     # UNIXROOT (D:) first: what RPM installs
export OS2X86_HOME=/h/OS2             # x86 OS/2 programs' OS2.INI and OS2SYS.INI: on the stick (HOME is RAM)
export OS2X86_PRINT=/h/PRINT          # what x86 OS/2 programs print: PDF files, in D:\\PRINT
mkdir -p /tmp/.cache                    # fuzzel remembers what was launched
cd /tmp
