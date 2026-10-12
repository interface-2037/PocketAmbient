#!/usr/bin/env bash
# autobuild.sh - local hardware-build loop for cloud work (run from the repo root, Git Bash).
#
#   tools/autobuild.sh            build origin/$BRANCH if it moved, install on the SD card if mounted
#   tools/autobuild.sh --status   print the state only
#
# The cloud pushes to one of $BRANCHES (default "cloud/next claude/v041"; cloud sessions may only be
# allowed to push claude/* branches). The branch with the newest commit wins. This script keeps a
# separate git worktree at $WT, so the main checkout is never touched. Per new commit it:
#   1. runs the Quartus build if src/fpga/ or pkg/ changed (or there is no bitstream yet),
#      and fails on any negative slack in output_files/ap_core.sta.summary;
#   2. packages release + dev zips (tools/package.py) into dist/autobuild/<version>-<sha>/;
#   3. if the Pocket card is mounted ($SD, label POCKET), backs up the core folder and the
#      firmware asset to dist/autobuild/sd_backup_<time>/, installs the dev build and verifies it.
# The last line printed is one RESULT line for the caller (the scheduled task) to report.
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BRANCHES="${BRANCHES:-cloud/next claude/v041}"
WT="${WT:-$REPO/../PocketAmbient-autobuild}"
SD="${SD:-/h}"
QUARTUS_BIN="${QUARTUS_BIN:-/c/altera_lite/25.1std/quartus/bin64}"
OUT="$REPO/dist/autobuild"
STATE="$OUT/state"                      # lines: built=<sha> installed=<sha> version=<v>
mkdir -p "$OUT"
touch "$STATE"
get() { sed -n "s/^$1=//p" "$STATE" | tail -1; }
put() { grep -v "^$1=" "$STATE" > "$STATE.tmp"; echo "$1=$2" >> "$STATE.tmp"; mv "$STATE.tmp" "$STATE"; }
result() { echo "RESULT: $*"; exit 0; }
fail() { echo "RESULT: FAILED - $*"; exit 1; }

if [ "${1:-}" = "--status" ]; then cat "$STATE"; exit 0; fi

# one run at a time (a Quartus build takes ~15 min)
LOCK="$OUT/lock"
if ! mkdir "$LOCK" 2>/dev/null; then result "skipped, another autobuild run is in progress ($LOCK)"; fi
trap 'rmdir "$LOCK" 2>/dev/null' EXIT

git -C "$REPO" fetch -q origin || fail "git fetch failed"
TARGET=""; BEST=0
for b in $BRANCHES; do
    sha="$(git -C "$REPO" rev-parse -q --verify "origin/$b")" || continue
    t="$(git -C "$REPO" log -1 --format=%ct "$sha")"
    if [ "$t" -gt "$BEST" ]; then BEST=$t; TARGET=$sha; BRANCH=$b; fi
done
[ -n "$TARGET" ] || result "none of origin/{${BRANCHES// /,}} exists yet, nothing to build"
SHORT="${TARGET:0:7}"
BUILT="$(get built)"
INSTALLED="$(get installed)"

if [ ! -d "$WT" ]; then
    git -C "$REPO" worktree add -q --detach "$WT" "$TARGET" || fail "worktree add failed"
fi

# ---------------------------------------------------------------- build
if [ "$TARGET" != "$BUILT" ]; then
    git -C "$WT" checkout -q --detach --force "$TARGET" || fail "checkout $SHORT failed"
    git -C "$WT" clean -qfd -e output_files -e build -e dist || true
    VERSION="$(python -c "import json;print(json.load(open(r'$WT/pkg/core.json'))['core']['metadata']['version'])")"
    NEED_FPGA=1
    if [ -n "$BUILT" ] && [ -f "$WT/src/fpga/output_files/ap_core.rbf" ] && \
       git -C "$REPO" cat-file -e "$BUILT" 2>/dev/null && \
       git -C "$REPO" diff --quiet "$BUILT" "$TARGET" -- src/fpga pkg; then
        NEED_FPGA=0
    fi
    mkdir -p "$WT/build"
    if [ $NEED_FPGA = 1 ]; then
        echo "Quartus build of $SHORT (v$VERSION) ..."
        LOG="$WT/build/fpga_$SHORT.log"
        ( cd "$WT/src/fpga" && PATH="$QUARTUS_BIN:$PATH" quartus_sh --flow compile ap_core ) > "$LOG" 2>&1 \
            || fail "Quartus build of $SHORT failed, see $LOG ($(grep -m1 '^Error' "$LOG"))"
        SUM="$WT/src/fpga/output_files/ap_core.sta.summary"
        WORST="$(sed -n 's/^Slack *: *//p' "$SUM" | sort -g | head -1)"
        case "$WORST" in -*) fail "timing NOT met in $SHORT: worst slack $WORST ns (see $SUM)";; esac
        FIT="$(grep -E 'Logic utilization|Total RAM Blocks' "$WT/src/fpga/output_files/ap_core.fit.summary" | sed 's/^ *//' | tr '\n' ';')"
    else
        echo "No RTL or package change since ${BUILT:0:7}: packaging only"
        WORST="(unchanged bitstream)"; FIT=""
    fi
    DEST="$OUT/$VERSION-$SHORT"
    rm -rf "$DEST"; mkdir -p "$DEST"
    ( cd "$WT" && python tools/package.py > /dev/null && cp dist/AmbientPocket-"$VERSION".zip "$DEST/" \
      && python tools/package.py --dev > /dev/null && cp dist/AmbientPocket-"$VERSION"-dev.zip "$DEST/" \
      && cp -r dist/sdcard "$DEST/sdcard" ) || fail "packaging $SHORT failed"
    [ -f "$WT/docs/HW_TEST_NEXT.md" ] && cp "$WT/docs/HW_TEST_NEXT.md" "$DEST/"
    put built "$TARGET"; put version "$VERSION"
    echo "built v$VERSION ($SHORT, $BRANCH): worst slack $WORST; $FIT"
    BUILT="$TARGET"
fi

# ---------------------------------------------------------------- install on the SD card
VERSION="$(get version)"
DEST="$OUT/$VERSION-${BUILT:0:7}"
if [ "$BUILT" = "$INSTALLED" ]; then result "v$VERSION (${BUILT:0:7}) already built and on the SD card; nothing new"; fi
LABEL="$(powershell.exe -NoProfile -Command "(Get-Volume -DriveLetter ${SD#/} -ErrorAction SilentlyContinue).FileSystemLabel" 2>/dev/null | tr -d '\r')"
if [ ! -d "$SD/Cores" ] || [ "$LABEL" != "POCKET" ]; then
    result "v$VERSION (${BUILT:0:7}) built in $DEST; SD card not mounted, will install on a later run"
fi
BK="$OUT/sd_backup_$(date +%Y%m%d-%H%M%S)"
mkdir -p "$BK"
cp -r "$SD/Cores/beingFar.AmbientPocket" "$BK/" 2>/dev/null
[ -d "$SD/Assets/ambient" ] && cp -r "$SD/Assets/ambient" "$BK/Assets_ambient"
cp -r "$DEST/sdcard/." "$SD/" || fail "copy to SD failed (backup in $BK)"
sync
for f in Cores/beingFar.AmbientPocket/bitstream.rbf_r Cores/beingFar.AmbientPocket/core.json \
         Cores/beingFar.AmbientPocket/data.json Assets/ambient/common/composer.bin; do
    cmp -s "$DEST/sdcard/$f" "$SD/$f" || fail "SD verify failed on $f (backup in $BK)"
done
put installed "$BUILT"
result "v$VERSION (${BUILT:0:7}) INSTALLED on the SD card (dev build, verified; backup in $BK). Eject the card and test."
