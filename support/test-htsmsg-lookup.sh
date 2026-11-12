#!/bin/sh
# Run after configuring the server. Link real lookup/constructor/destructor code.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${BUILDDIR:-"$root/build.linux"}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cc=${CC:-cc}
"$cc" -O2 -g -ffunction-sections -fdata-sections -fms-extensions -funsigned-char \
  ${CFLAGS:-} -I"$build" -I"$root/src" -I"$root" \
  -c "$root/src/htsmsg.c" -o "$tmp/htsmsg.o"
"$cc" -O2 -g -fms-extensions -funsigned-char ${CFLAGS:-} \
  -I"$build" -I"$root/src" -I"$root" "$root/support/test-htsmsg-lookup.c" \
  "$tmp/htsmsg.o" -Wl,--gc-sections ${LDFLAGS:-} -o "$tmp/test-lookup"
"$tmp/test-lookup"
