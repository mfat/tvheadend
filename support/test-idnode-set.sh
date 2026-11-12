#!/bin/sh
# Run after configuring Tvheadend. Compile the real idnode implementation,
# discarding unrelated sections and replacing only the filter dependency.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${BUILDDIR:-"$root/build.linux"}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cc=${CC:-cc}
"$cc" -O2 -g -ffunction-sections -fdata-sections -fms-extensions \
  -funsigned-char ${CFLAGS:-} -I"$build" -I"$root/src" -I"$root" \
  -c "$root/src/idnode.c" -o "$tmp/idnode.o"
# A strong test definition supplies accept/reject decisions; insertion,
# capacity growth and the data structures come from production code.
${OBJCOPY:-objcopy} --weaken-symbol=idnode_filter "$tmp/idnode.o"
"$cc" -O2 -g -fms-extensions -funsigned-char -D_GNU_SOURCE ${CFLAGS:-} \
  -I"$build" -I"$root/src" -I"$root" "$root/support/test-idnode-set.c" \
  "$tmp/idnode.o" -Wl,--gc-sections ${LDFLAGS:-} -o "$tmp/test-idnode-set"
"$tmp/test-idnode-set"
