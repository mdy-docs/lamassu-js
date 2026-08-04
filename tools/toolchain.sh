# Pinned wasm toolchains: where they are, and where they come from.
#
# Sourced by tools/get-wasi-sdk.sh and tools/get-wasmtime.sh; not executable
# on its own. The Makefile reads WASI_SDK_VERSION straight out of this file
# (see its `wasi-sdk-check` section), so the pins below are the ONE place each
# version is written down — a bump is one line here, and a CI runner and a
# developer's machine cannot end up on different toolchains, which is the whole
# reason anything is pinned: a codegen change should arrive as a commit, not as
# a Tuesday.
#
# Adapted from the sibling nisaba-db repo's wasm/build-common.sh, which solved
# this first; the discovery order, the atomic unpack and the stdout/stderr
# split are all its design.

WASI_SDK_VERSION=33.0
WASMTIME_VERSION=47.0.2

# macos|linux. Fails, loudly, rather than guessing for a platform these
# projects publish no asset for.
host_os() {
  case "$(uname -s)" in
    Darwin) printf 'macos\n' ;;
    Linux)  printf 'linux\n' ;;
    *) echo "error: no pinned toolchain build for $(uname -s)" >&2; return 1 ;;
  esac
}

# arm64|x86_64, canonically — each project spells arm64 its own way, so the
# callers below translate rather than this.
host_arch() {
  # uname -m says x86_64 both on real Intel silicon and in an x86_64 shell
  # running under Rosetta on an arm64 Mac; only proc_translated tells them
  # apart, and it is absent (not 0) everywhere else.
  if [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || echo 0)" = 1 ]; then
    printf 'arm64\n'
    return 0
  fi
  case "$(uname -m)" in
    arm64|aarch64) printf 'arm64\n' ;;
    x86_64|amd64)  printf 'x86_64\n' ;;
    *) echo "error: no pinned toolchain build for $(uname -m)" >&2; return 1 ;;
  esac
}

# Where the fetch scripts install by default: BESIDE the repository, not inside
# it. One download serves every worktree rather than being re-fetched per
# checkout, and a `git clean -xdf` cannot cost anyone a 600MB download.
# Override with $TOOLCHAIN_HOME, or per-toolchain below.
toolchain_home() {
  printf '%s\n' "${TOOLCHAIN_HOME:-$(dirname "$PWD")}"
}

# Download and unpack a pinned toolchain into <dest>.
#
# Unpacks into a staging directory and moves it into place at the end, so an
# interrupted download can never leave something that LOOKS like a toolchain —
# the discovery below would find it and every later build would fail somewhere
# further in. Compression is left to tar to detect, which both bsdtar and GNU
# tar do, because the two projects publish .tar.gz and .tar.xz respectively.
#   fetch_unpack <url> <dest>
fetch_unpack() {
  url="$1"; dest="$2"
  command -v curl >/dev/null 2>&1 || { echo "error: curl is needed to fetch $url" >&2; return 1; }
  tmp="$(mktemp "${TMPDIR:-/tmp}/toolchain-XXXXXX.tar")"
  stage="$dest.partial.$$"
  # shellcheck disable=SC2064 -- expand now: $tmp/$stage are this call's
  trap "rm -rf -- '$tmp' '$stage'" EXIT
  curl -fL --progress-bar -o "$tmp" "$url" >&2
  mkdir -p "$stage"
  tar xf "$tmp" -C "$stage" --strip-components=1
  rm -rf -- "$dest"
  mkdir -p "$(dirname "$dest")"
  mv "$stage" "$dest"
  rm -f -- "$tmp"
  trap - EXIT
}

# ---- wasi-sdk ----
#
# The release tag and the asset name spell the version differently — tag
# wasi-sdk-33, file wasi-sdk-33.0-<arch>-<os>.tar.gz — so both are derived here
# from the constant above rather than written out twice.
wasi_sdk_url() {
  printf 'https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-%s/wasi-sdk-%s-%s.tar.gz\n' \
    "${WASI_SDK_VERSION%%.*}" "$WASI_SDK_VERSION" "$1"
}

# The <arch>-<os> this host needs, in wasi-sdk's naming (arm64-macos).
wasi_sdk_platform() { printf '%s-%s\n' "$(host_arch)" "$(host_os)"; }

wasi_sdk_home() { printf '%s\n' "${WASI_SDK_HOME:-$(toolchain_home)}"; }

# Every place a wasi-sdk might be, most deliberate first. Keep in step with the
# Makefile's own search list, which covers the last two.
wasi_sdk_candidates() {
  [ -n "${WASI_SDK:-}" ] && printf '%s\n' "$WASI_SDK"
  printf '%s\n' "$(wasi_sdk_home)/wasi-sdk-$WASI_SDK_VERSION"
  printf '%s\n' /opt/wasi-sdk
}

# Print the first candidate that is actually a wasi-sdk — a clang and a
# sysroot, which is all these builds need from it. Nonzero and silent if there
# is none; the caller reports that, because only it knows what was being tried.
find_wasi_sdk() {
  # Falling through to the next candidate is right — a stale variable in a
  # shell profile should not stop a build that has a perfectly good toolchain
  # installed — but doing it quietly is not: whoever set it is owed the reason
  # their choice was not used.
  if [ -n "${WASI_SDK:-}" ] &&
     { [ ! -x "$WASI_SDK/bin/clang" ] || [ ! -d "$WASI_SDK/share/wasi-sysroot" ]; }; then
    echo "warning: \$WASI_SDK=$WASI_SDK is not a wasi-sdk (no bin/clang + share/wasi-sysroot); looking elsewhere" >&2
  fi
  wasi_sdk_candidates | while IFS= read -r dir; do
    if [ -x "$dir/bin/clang" ] && [ -d "$dir/share/wasi-sysroot" ]; then
      printf '%s\n' "$dir"
      return 0
    fi
  done | head -n 1 | grep . || return 1
}

# ---- wasmtime ----

wasmtime_url() {
  printf 'https://github.com/bytecodealliance/wasmtime/releases/download/v%s/wasmtime-v%s-%s.tar.xz\n' \
    "$WASMTIME_VERSION" "$WASMTIME_VERSION" "$1"
}

# wasmtime spells arm64 "aarch64", where wasi-sdk spells it "arm64" — the
# reason host_arch canonicalizes and each caller translates.
wasmtime_platform() {
  arch="$(host_arch)"
  [ "$arch" = arm64 ] && arch=aarch64
  printf '%s-%s\n' "$arch" "$(host_os)"
}

wasmtime_home() { printf '%s\n' "${WASMTIME_HOME:-$(toolchain_home)}"; }

wasmtime_candidates() {
  [ -n "${WASMTIME:-}" ] && printf '%s\n' "$WASMTIME"
  printf '%s\n' "$(wasmtime_home)/wasmtime-$WASMTIME_VERSION/wasmtime"
  printf '%s\n' /opt/wasmtime/wasmtime
  command -v wasmtime 2>/dev/null || true
}

find_wasmtime() {
  wasmtime_candidates | while IFS= read -r exe; do
    if [ -x "$exe" ]; then printf '%s\n' "$exe"; return 0; fi
  done | head -n 1 | grep . || return 1
}

# Say so when the wasmtime found is not the pinned one — most likely a package
# manager's, which moves on its own schedule. A warning, not an error: a
# deliberate override is allowed, but a version difference is the likeliest
# reason for a result that reproduces on one machine and not the other.
warn_unpinned_wasmtime() {
  found="$("$1" --version 2>/dev/null | awk 'NR==1 {print $2}')" || return 0
  if [ -n "$found" ] && [ "$found" != "$WASMTIME_VERSION" ]; then
    echo "warning: $1 is wasmtime $found, not the pinned $WASMTIME_VERSION (CI runs the pin)" >&2
  fi
}
