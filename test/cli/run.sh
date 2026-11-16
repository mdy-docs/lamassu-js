#!/bin/sh
# The command line tool, run over the cases here. A case is a directory: its entry main.mjs (or
# main.js), `expect` what the run prints, `status` its exit status where that is not 0, and
# `stderr` a line that its standard error has in it.
#
# Each is run three ways -- named from the repository's root, from inside its own directory, and
# by its absolute path -- because where a module is comes from how the entry was named.
#
#   sh test/cli/run.sh build/lamassu       (make cli-check)
bin=$1
case "$bin" in /*) ;; *) bin=$(pwd)/$bin ;; esac
root=$(pwd)
err=$(mktemp)
trap 'rm -f "$err"' EXIT
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
if [ "$failed" != 0 ]; then
  echo "$failed of $count command line runs failed"
  exit 1
fi
echo "$count command line runs ok"
