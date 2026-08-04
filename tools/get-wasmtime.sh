#!/usr/bin/env bash
# Fetch the pinned wasmtime for this host and print where it is, so that
# `make test-wasi` and `make bench-wasi` have a runtime to use.
#
#   ./tools/get-wasmtime.sh              fetch if missing, print the path
#   ./tools/get-wasmtime.sh --dir DIR    install into DIR instead
#   ./tools/get-wasmtime.sh --force      re-fetch over an existing copy
#   ./tools/get-wasmtime.sh --print-url  print the asset URL and stop
#
# Everything here mirrors tools/get-wasi-sdk.sh: no-op when a usable copy is
# already there, path on stdout and messages on stderr, and the version comes
# from tools/toolchain.sh rather than from this file.
#
# Unlike the wasi-sdk this one is also satisfied by whatever is on PATH, so a
# developer with a package-manager wasmtime needs nothing — they just get a
# warning when its version is not the pinned one, because a version difference
# is the likeliest reason for a result that reproduces on one machine and not
# the other.
set -euo pipefail

cd "$(dirname "$0")/.."
. tools/toolchain.sh

DEST=""; FORCE=0; PRINT_URL=0
while [ $# -gt 0 ]; do
  case "$1" in
    --dir)       DEST="${2:?--dir needs a path}"; shift 2 ;;
    --force)     FORCE=1; shift ;;
    --print-url) PRINT_URL=1; shift ;;
    *) echo "usage: $0 [--dir DIR] [--force] [--print-url]" >&2; exit 2 ;;
  esac
done

PLATFORM="$(wasmtime_platform)"
URL="$(wasmtime_url "$PLATFORM")"

if [ "$PRINT_URL" = 1 ]; then echo "$URL"; exit 0; fi

if [ "$FORCE" = 0 ]; then
  if [ -n "$DEST" ]; then
    if [ -x "$DEST/wasmtime" ]; then
      echo "wasmtime already at $DEST/wasmtime" >&2
      echo "$DEST/wasmtime"; exit 0
    fi
  elif FOUND="$(find_wasmtime)"; then
    warn_unpinned_wasmtime "$FOUND"
    echo "wasmtime already at $FOUND" >&2
    echo "$FOUND"; exit 0
  fi
fi

[ -n "$DEST" ] || DEST="$(wasmtime_home)/wasmtime-$WASMTIME_VERSION"

echo "fetching wasmtime $WASMTIME_VERSION ($PLATFORM)" >&2
echo "  from $URL" >&2
echo "  into $DEST" >&2
fetch_unpack "$URL" "$DEST"

if [ ! -x "$DEST/wasmtime" ]; then
  echo "error: $URL unpacked without a wasmtime executable" >&2
  exit 1
fi
echo "installed wasmtime $WASMTIME_VERSION at $DEST/wasmtime" >&2
echo "$DEST/wasmtime"
