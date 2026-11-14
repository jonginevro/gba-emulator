#!/bin/sh
# Runs jsmolka's gba-suite against the emulator. Each ROM leaves the number of the
# first failed test in r12, so r12 == 0 means everything passed.
set -e

cd "$(dirname "$0")"
ROMS=roms
BASE=https://raw.githubusercontent.com/jsmolka/gba-suite/master
GBA=../gba

[ -x "$GBA" ] || { echo "build the emulator first (make)"; exit 1; }

mkdir -p $ROMS
for t in arm/arm thumb/thumb memory/memory save/none save/sram save/flash64 save/flash128; do
	name=$(basename $t)
	[ -f "$ROMS/$name.gba" ] || curl -sSL -o "$ROMS/$name.gba" "$BASE/$t.gba"
done

rc=0
for rom in $ROMS/*.gba; do
	name=$(basename "$rom" .gba)
	rm -f "$ROMS/$name.sav"
	r12=$($GBA --frames 300 "$rom" | head -1 | awk '{print $14}')
	if [ "$r12" = "00000000" ]; then
		echo "pass  $name"
	else
		num=$(printf '%d' "0x$r12" 2>/dev/null || echo "0x$r12")
		echo "FAIL  $name (test $num)"
		rc=1
	fi
done

exit $rc
