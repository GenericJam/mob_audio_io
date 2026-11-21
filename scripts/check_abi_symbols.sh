#!/usr/bin/env bash
# check_abi_symbols.sh OBJECT [NM]
#
# Every function priv/include/mob_audio_io.h declares must be defined (T) in
# OBJECT, a compiled platform NIF. CI runs it on the Android object and the
# iOS object, so a symbol missing on one platform fails here instead of at an
# app's link (mob_dev has no such check yet: MOB-542).
set -euo pipefail

obj="$1"
nm_bin="${2:-nm}"
header="$(dirname "$0")/../priv/include/mob_audio_io.h"

# Declarations: "int32_t mai_name(" at the start of a line.
symbols=$(grep -oE '^[a-z0-9_]+ +\*?mai_[a-z_]+\(' "$header" | sed -E 's/.*(mai_[a-z_]+)\(/\1/' | sort -u)
if [ -z "$symbols" ]; then
    echo "no mai_* declarations found in $header" >&2
    exit 1
fi

defined=$("$nm_bin" -g "$obj" | awk '$2 == "T" { sub(/^_/, "", $3); print $3 }')
missing=0
for s in $symbols; do
    if ! grep -qx "$s" <<<"$defined"; then
        echo "missing in $obj: $s" >&2
        missing=1
    fi
done
[ "$missing" = 0 ] && echo "$obj defines: $(echo $symbols)"
exit "$missing"
