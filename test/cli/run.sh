#!/bin/sh
# The command line tool, run over the cases here. A case is a directory: its entry main.mjs (or
# main.js), `expect` what the run prints, `status` its exit status where that is not 0, and
# `stderr` a line that its standard error has in it.
#
# Each is run three ways -- named from the repository's root, from inside its own directory, and
# by its absolute path -- because where a module is comes from how the entry was named.
#
# After the cases, the limits: --fuel ends a run whichever way the program was given.
#
#   sh test/cli/run.sh build/lamassu       (make cli-check)
bin=$1
case "$bin" in /*) ;; *) bin=$(pwd)/$bin ;; esac
root=$(pwd)
err=$(mktemp)
tmp=$(mktemp -d)
trap 'rm -f "$err" "$tmp"/spin.mjs "$tmp"/spin.jsbc; rmdir "$tmp"' EXIT
failed=0
count=0
for dir in test/cli/*/; do
  dir=${dir%/}
  entry=main.mjs
  [ -f "$dir/main.js" ] && entry=main.js
  want_status=0
  [ -f "$dir/status" ] && want_status=$(tr -d '\r\n' < "$dir/status")
  want=$(tr -d '\r' < "$dir/expect")
  for how in root inside absolute; do
    case $how in
      root) got=$("$bin" "$dir/$entry" 2> "$err"); status=$? ;;
      inside) got=$(cd "$dir" && "$bin" "$entry" 2> "$err"); status=$? ;;
      absolute) got=$("$bin" "$root/$dir/$entry" 2> "$err"); status=$? ;;
    esac
    got=$(printf '%s\n' "$got" | tr -d '\r')
    why=
    if grep -q 'Sanitizer\|runtime error' "$err"; then
      why="the sanitizer stopped it"
    elif [ "$status" != "$want_status" ]; then
      why="exit status $status, expected $want_status"
    elif [ "$got" != "$want" ]; then
      why="printed
$got
-- expected
$want"
    elif [ -f "$dir/stderr" ] && ! grep -qF "$(tr -d '\r\n' < "$dir/stderr")" "$err"; then
      why="standard error has no '$(tr -d '\r\n' < "$dir/stderr")'"
    fi
    count=$((count + 1))
    if [ -n "$why" ]; then
      failed=$((failed + 1))
      echo "FAIL $dir ($how): $why"
      sed 's/^/  stderr: /' "$err"
    fi
  done
done

# --fuel is the budget of a source file and of a bytecode cache alike: a cache with an export
# is a module's (loaded through the module loader), one without is a function's, and each must
# stop. The loops end by themselves, so a budget that is not armed fails here and does not hang.
for body in 'export let n = 0; for (let i = 0; i < 5e7; i++) n++;' \
            'let n = 0; for (let i = 0; i < 5e7; i++) n++;'; do
  printf '%s\n' "$body" > "$tmp/spin.mjs"
  for how in source cache; do
    case $how in
      source) "$bin" --fuel 100000 "$tmp/spin.mjs" > /dev/null 2> "$err"; status=$? ;;
      cache) "$bin" --emit-bytecode "$tmp/spin.mjs" "$tmp/spin.jsbc" > /dev/null 2> "$err" &&
               "$bin" --fuel 100000 --run-bytecode "$tmp/spin.jsbc" > /dev/null 2> "$err"; status=$? ;;
    esac
    why=
    if grep -q 'Sanitizer\|runtime error' "$err"; then
      why="the sanitizer stopped it"
    elif [ "$status" != 1 ]; then
      why="exit status $status, expected 1"
    elif ! grep -qF 'uncaught RangeError: execution budget exhausted' "$err"; then
      why="standard error has no 'uncaught RangeError: execution budget exhausted'"
    fi
    count=$((count + 1))
    if [ -n "$why" ]; then
      failed=$((failed + 1))
      echo "FAIL --fuel ($how of: $body): $why"
      sed 's/^/  stderr: /' "$err"
    fi
  done
done

if [ "$failed" != 0 ]; then
  echo "$failed of $count command line runs failed"
  exit 1
fi
echo "$count command line runs ok"
