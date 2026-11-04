#!/usr/bin/env bash
# Lazarus: HOME-menu NSP forwarder for the NRO (docs/forwarder.md). Its NPDM allows CPU core 3, which a plain
# homebrew launch does not give, and it asks the HOME menu for a user so the game saves in that user's save data.
#
#   KEYS=/path/to/prod.keys bash tools/nx/build_forwarder.sh [sdmc:/path/to.nro]
#
# KEYS: YOUR OWN console's prod.keys. Only header_key is read (to encrypt the NCA headers); the file is never copied.
# Never commit or share keys. The NSP needs sigpatches like any homebrew NSP.
# Loader: Forwarder-Mod (nx-hbloader) 9853bd9, built with /opt/devkitpro.
set -euo pipefail
NRO_SD="${1:-sdmc:/switch/me2-deko/me2_opt.nro}"
SRC=$(cd "$(dirname "$0")/../.." && pwd)
KEYS=${KEYS:?set KEYS to your own prod.keys}
OUT=${OUT:-$SRC/out/nsp}
TID="${TITLE_ID:-014D453244000000}"   # 01 + "ME2D" + 0000 (= me2_switch_save_title_id in the toml)
ICON=${ICON:-$SRC/forwarder/icon_lazarus.jpg}
W=${FORWARDER_WORK:-$HOME/lazarus-forwarder}

[ -f "$KEYS" ] || { echo "no $KEYS"; exit 1; }
mkdir -p "$W" "$OUT"
cp "$SRC"/forwarder/build_forwarder.py "$SRC"/forwarder/nsp_container.py "$W"/
cp "$ICON" "$W/icon.jpg"
if [ ! -d "$W/loader/.git" ]; then
  git clone -q https://github.com/Skywalker25/Forwarder-Mod.git "$W/loader"
fi
git -C "$W/loader" checkout -q 9853bd9167d5a7f0c5fdf935864245c1fbcf8282
# Current libnx no longer defines NORETURN.
sed -i 's/void NORETURN /void __attribute__((noreturn)) /' "$W/loader/source/main.c"
[ -f "$W/loader/hbl.json.orig" ] || cp "$W/loader/hbl.json" "$W/loader/hbl.json.orig"

export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
cd "$W"
# --save-mb / --journal-mb: the sizes app/me2_src/me2_switch_save.cpp uses (kSaveSize / kJournalSize).
python3 build_forwarder.py --nro "$NRO_SD" --icon icon.jpg --title "${TITLE:-Mass Effect 2 (Lazarus)}" \
  --author "${AUTHOR:-Lazarus}" --version 0.1.0 --tid "$TID" --keys "$KEYS" --out "$OUT" \
  --file-name "${FILE_NAME:-Lazarus}" --save-mb 64 --journal-mb 32
ls -la "$OUT"
