#!/bin/bash
# userland/serve-rpms.sh - the package repository, from the Mac or PC QEMU
# runs on: build/userland/rpms (userland/build-rpms.sh's) over HTTP, with
# the index.txt rpm reads. In the guest the host is 10.0.2.2, so the
# system's rpm finds it there by default:
#   rpm -qA                 what's here
#   rpm -i llvm             installs it (from \OS2\RPMS first, if it's there)
#   userland/serve-rpms.sh [port]       (default 8080)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/build/userland/rpms"
PORT="${1:-8080}"
[ -d "$DIR" ] || { echo "no $DIR - run userland/build-rpms.sh" >&2; exit 1; }
(cd "$DIR" && ls -1 *.rpm > index.txt)
echo "Serving $(wc -l < "$DIR/index.txt" | tr -d ' ') packages from $DIR on port $PORT (the guest: http://10.0.2.2:$PORT/)"
exec python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$DIR"
