#!/bin/bash
# Minimal repro: 40-level-deep save tree + POST /api/save/backup.
# Isolates the host-test crash (connection reset during /api/install/poll)
# from the 190-check harness so it can be run under gdb in one shot.
#
#   wsl.exe -d Ubuntu-22.04 bash .build/repro-deep-backup.sh
set -u
cd "$(wslpath 'C:\Users\songl\Desktop\Web File Manager\PS5-Mango')" || exit 9

VER=$(sed -n 's/^VERSION_TAG[[:space:]]*:*=[[:space:]]*\([^[:space:]]*\).*/\1/p' Makefile | head -1)
BIN="PS5-Mango-linux-$VER"
[ -x "$BIN" ] || { echo "FAIL: $BIN missing (run make linux first)" >&2; exit 9; }

R=/tmp/deep-repro
export WFM_SAVE_ROOT=$R/saveroot
export WFM_SNAPSHOT_DIR=$R/snaps
export WFM_HOMEBREW_DIR=$R/brew
export WFM_DEVICE_ROOT_PREFIX=$R/devroot
export WFM_SAVE_LOG=$R/save-delete.log

rm -rf "$R"
mkdir -p "$WFM_SAVE_ROOT/savedata_prospero/DEEP00003" "$WFM_SNAPSHOT_DIR" \
         "$WFM_HOMEBREW_DIR" "$WFM_DEVICE_ROOT_PREFIX"

cur="$WFM_SAVE_ROOT/savedata_prospero/DEEP00003"
for i in $(seq 0 39); do cur="$cur/d$i"; done
mkdir -p "$cur"
printf 'DEEP-LEAF' > "$cur/leaf.bin"

"./$BIN" > "$R/server.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT

PORT=""
for _ in $(seq 1 60); do
  for p in $(seq 2026 2040); do
    if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$p/api/status"; then
      PORT=$p; break
    fi
  done
  [ -n "$PORT" ] && break
  sleep 0.25
done
[ -n "$PORT" ] || { echo "server never came up"; cat "$R/server.log"; exit 1; }
echo "server pid=$SRV port=$PORT"

echo "--- POST /api/save/backup (DEEP00003, 40 levels) ---"
curl -s --max-time 5 -X POST \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  -d 'title_id=DEEP00003' "http://127.0.0.1:$PORT/api/save/backup"
echo

for _ in $(seq 1 30); do
  if ! curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/api/status"; then
    echo "SERVER DEAD after backup"
    break
  fi
  sleep 0.2
done

echo "--- final /api/status ---"
curl -s --max-time 3 "http://127.0.0.1:$PORT/api/status" && echo " (alive)" || echo "SERVER DEAD"
wait $SRV 2>/dev/null; RC=$?
echo "server exit code: $RC (139=SIGSEGV, 134=SIGABRT)"
echo "--- forensics tail ---"
tail -4 "$WFM_SAVE_LOG" 2>/dev/null
echo "--- server log ---"
cat "$R/server.log"
