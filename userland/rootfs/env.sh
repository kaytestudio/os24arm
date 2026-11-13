# env.sh - the environment for the Wayland session (sourced by desktop.sh
# and dwl.sh): fonts, keyboard layout ($1, variant $2; default us),
# compose sequences, paths. Everything lives on the stick at /h.
export FONTCONFIG_FILE=/h/fonts.conf
export XKB_CONFIG_ROOT=/h/xkb
export XKB_DEFAULT_LAYOUT="${1:-us}"
[ -n "$2" ] && export XKB_DEFAULT_VARIANT="$2"
export XCOMPOSEFILE=/h/Compose          # dead keys and compose sequences
export LANG=C.UTF-8
export XDG_RUNTIME_DIR=/tmp HOME=/tmp SHELL=/h/busybox
export PATH=/h
cd /tmp
