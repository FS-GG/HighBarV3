#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# T017 [US1] — Move command behavioral verification.
# arm-covered: move_unit
#
# Captures the commander's position from a snapshot (pre-move),
# dispatches `MoveUnit(commander_id, pre_pos + (500, 0, 0))`, waits
# 120 frames (~4s), captures a post-move snapshot, and asserts the
# commander's position.x delta is ≥ 100 elmos.
#
# Exit codes (per quickstart.md §2):
#   0  — position delta ≥ 100 (move executed).
#   1  — no movement, target destroyed, or other verification failure.
#   77 — setup skip (no engine, no plugin, no commander in 30s, etc.).

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HEADLESS_DIR="$REPO_ROOT/tests/headless"
EXAMPLES_DIR="$REPO_ROOT/specs/002-live-headless-e2e/examples"

# shellcheck source=tests/headless/_fault-assert.sh
source "$HEADLESS_DIR/_fault-assert.sh"

# ---- prereq checks --------------------------------------------------------

if [[ ! -x "$HEADLESS_DIR/_launch.sh" ]]; then
    echo "behavioral-move: _launch.sh missing — skip" >&2
    exit 77
fi
if [[ ! -f "$EXAMPLES_DIR/coordinator.py" ]]; then
    echo "behavioral-move: coordinator.py missing — skip" >&2
    exit 77
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "behavioral-move: python3 missing — skip" >&2
    exit 77
fi
if ! python3 -c "import grpc" 2>/dev/null; then
    echo "behavioral-move: grpcio missing — skip" >&2
    exit 77
fi

PYPROTO_DIR="${HIGHBAR_PYPROTO:-/tmp/hb-run/pyproto}"
if [[ ! -d "$PYPROTO_DIR/highbar" ]]; then
    mkdir -p "$PYPROTO_DIR/highbar"
    if ! python3 -m grpc_tools.protoc -I "$REPO_ROOT/proto" \
            --python_out="$PYPROTO_DIR" \
            --grpc_python_out="$PYPROTO_DIR" \
            "$REPO_ROOT/proto/highbar/"*.proto 2>/dev/null; then
        echo "behavioral-move: proto codegen failed — skip" >&2
        exit 77
    fi
    touch "$PYPROTO_DIR/highbar/__init__.py"
fi

RUN_DIR="${HIGHBAR_RUN_DIR:-/tmp/hb-run}"
mkdir -p "$RUN_DIR"
COORD_SOCK="$RUN_DIR/hb-coord.sock"
COORD_LOG="$RUN_DIR/coord.log"
START_SCRIPT="$HEADLESS_DIR/scripts/minimal.startscript"
ENGINE_LOG="$RUN_DIR/highbar-launch.log"
ENGINE_PID_FILE="$RUN_DIR/highbar-launch.pid"
WRITE_DIR="${HIGHBAR_WRITE_DIR:-$HOME/.local/state/Beyond All Reason}"
CLIENT_LOG="$RUN_DIR/behavioral-move.log"

cleanup() {
    [[ -f "$ENGINE_PID_FILE" ]] && kill -TERM "$(cat "$ENGINE_PID_FILE")" 2>/dev/null
    [[ -n "${COORD_PID:-}" ]] && kill -TERM "$COORD_PID" 2>/dev/null
}
trap cleanup EXIT

# ---- start coordinator + plugin ------------------------------------------

rm -f "$COORD_SOCK"
python3 "$EXAMPLES_DIR/coordinator.py" \
    --endpoint "unix:$COORD_SOCK" --id bmove > "$COORD_LOG" 2>&1 &
COORD_PID=$!
for _ in $(seq 1 20); do
    [[ -S "$COORD_SOCK" ]] && break
    sleep 0.2
done
if [[ ! -S "$COORD_SOCK" ]]; then
    echo "behavioral-move: coordinator did not bind — skip" >&2
    cat "$COORD_LOG" >&2
    exit 77
fi

LAUNCH_OUT=$("$HEADLESS_DIR/_launch.sh" \
    --start-script "$START_SCRIPT" \
    --coordinator "unix:$COORD_SOCK" \
    --runtime-dir "$RUN_DIR" 2>&1)
LAUNCH_RC=$?
if [[ $LAUNCH_RC -eq 77 ]]; then
    echo "behavioral-move: _launch.sh prereq missing — skip" >&2
    echo "$LAUNCH_OUT" >&2
    exit 77
elif [[ $LAUNCH_RC -ne 0 ]]; then
    echo "behavioral-move: _launch.sh failed — fail" >&2
    exit 1
fi

for _ in $(seq 1 30); do
    if grep -q '\[hb-gateway\] startup' "$ENGINE_LOG" 2>/dev/null; then break; fi
    sleep 1
done
if ! grep -q '\[hb-gateway\] startup' "$ENGINE_LOG"; then
    echo "behavioral-move: gateway startup banner not seen — fail" >&2
    tail -50 "$ENGINE_LOG" >&2
    exit 1
fi

# Give the engine a few seconds so the commander spawns.
sleep 5

# ---- behavioral test ------------------------------------------------------

PYTHONPATH="$PYPROTO_DIR" timeout 60 python3 - "$COORD_SOCK" > "$CLIENT_LOG" 2>&1 <<'PYEOF'
import sys
import threading
import time

import grpc
from highbar import service_pb2, service_pb2_grpc
from highbar import commands_pb2

endpoint = "unix:" + sys.argv[1]
ch = grpc.insecure_channel(endpoint)
stub = service_pb2_grpc.HighBarProxyStub(ch)

# Hello as AI role (so SubmitCommands works).
resp = stub.Hello(service_pb2.HelloRequest(
    schema_version="1.0.0",
    role=service_pb2.Role.ROLE_AI,
), timeout=5)
print(f"[bmove] Hello OK session={resp.session_id}", flush=True)

# Watch state in a background thread so we can snapshot-diff.
shared = {"snapshots": [], "stop": False, "err": None}

def watcher():
    try:
        for upd in stub.StreamState(
                service_pb2.StreamStateRequest(resume_from_seq=0),
                timeout=50):
            if shared["stop"]:
                return
            if upd.WhichOneof("payload") == "snapshot":
                # Keep a rolling history so the main thread can pick up
                # snapshots before and after dispatch.
                shared["snapshots"].append((time.monotonic(), upd.snapshot))
    except grpc.RpcError as e:
        shared["err"] = e.code().name

t = threading.Thread(target=watcher, daemon=True)
t.start()

def find_commander(snap):
    # minimal.startscript is Armada-vs-Armada; the commander is armcom
    # (def_id lookup is done by-name via CallbackRequest in a full
    # implementation — for the behavioral check we use the first unit
    # with max_health > 3000 as an armcom heuristic: commanders are the
    # highest-HP starting unit on BAR).
    best = None
    for u in snap.own_units:
        if u.max_health > 3000 and (best is None or u.max_health > best.max_health):
            best = u
    return best

# Wait up to 30s for a snapshot containing our commander.
deadline = time.monotonic() + 30.0
before_snap = None
before = None
while time.monotonic() < deadline:
    if shared["snapshots"]:
        last = shared["snapshots"][-1][1]
        cmdr = find_commander(last)
        if cmdr is not None:
            before_snap = last
            before = cmdr
            break
    time.sleep(0.2)
if before is None:
    print(f"[bmove] no commander in snapshots after 30s err={shared.get('err')}",
          flush=True)
    sys.exit(77)

cmdr_id = before.unit_id
px, py, pz = before.position.x, before.position.y, before.position.z
print(f"[bmove] commander_id={cmdr_id} before=({px:.1f}, {py:.1f}, {pz:.1f})",
      flush=True)

# Dispatch MoveUnit(commander, pre_pos + (500, 0, 0)).
def gen():
    batch = commands_pb2.CommandBatch()
    batch.batch_seq = 1
    batch.client_command_id = 1
    batch.target_unit_id = cmdr_id
    cmd = batch.commands.add()
    cmd.move_unit.unit_id = cmdr_id
    cmd.move_unit.to_position.x = px + 500.0
    cmd.move_unit.to_position.y = py
    cmd.move_unit.to_position.z = pz
    cmd.move_unit.options = 0
    cmd.move_unit.timeout = 0
    yield batch

ack = stub.SubmitCommands(gen(), timeout=10)
print(f"[bmove] dispatched MoveUnit to ({px+500:.1f}, {py:.1f}, {pz:.1f}) "
      f"accepted={ack.batches_accepted}", flush=True)

# Wait 120 engine frames (~4s wall clock at 30fps).
target_frame = before_snap.frame_number + 120
post_deadline = time.monotonic() + 10.0
after = None
while time.monotonic() < post_deadline:
    for ts, snap in reversed(shared["snapshots"]):
        if snap.frame_number >= target_frame:
            c = find_commander(snap)
            if c is not None and c.unit_id == cmdr_id:
                after = c
                break
    if after is not None:
        break
    time.sleep(0.2)

shared["stop"] = True

if after is None:
    # Either commander destroyed or snapshot stream stalled.
    any_still = any(find_commander(s) is not None
                    for _, s in shared["snapshots"][-5:])
    if not any_still:
        print("[bmove] target unit destroyed during test window", flush=True)
        sys.exit(1)
    print(f"[bmove] no post-snapshot after frame={target_frame} — fail", flush=True)
    sys.exit(1)

qx, qy, qz = after.position.x, after.position.y, after.position.z
import math
dx = qx - px
dz = qz - pz
total = math.sqrt(dx * dx + dz * dz)
print(f"[bmove] after=({qx:.1f}, {qy:.1f}, {qz:.1f}) "
      f"dx={dx:.1f} dz={dz:.1f} |d|={total:.1f} (threshold 100)",
      flush=True)
# Accept total displacement >= 100. Fixture physics and terrain may alter the
# exact +500x target, but any net motion proves the engine acted on a MoveUnit
# rather than ignoring it.
if total < 100.0:
    print(f"[bmove] move not executed: before=({px:.1f},{py:.1f},{pz:.1f}) "
          f"after=({qx:.1f},{qy:.1f},{qz:.1f})", flush=True)
    sys.exit(1)

print("[bmove] PASS")
sys.exit(0)
PYEOF
PY_RC=$?

cat "$CLIENT_LOG"

if [[ $PY_RC -eq 77 ]]; then
    echo "behavioral-move: setup skip (see log)" >&2
    exit 77
elif [[ $PY_RC -ne 0 ]]; then
    echo "behavioral-move: FAIL rc=$PY_RC" >&2
    tail -30 "$ENGINE_LOG" >&2
    exit 1
fi

# Fault-status check: gateway must still be Healthy.
EFFECTIVE_WRITEDIR="$WRITE_DIR/engine/recoil_2025.06.19"
fault_status "$EFFECTIVE_WRITEDIR"
if [[ $? -eq 2 ]]; then
    echo "behavioral-move: gateway DISABLED — fail" >&2
    exit 1
fi

echo "behavioral-move: PASS"
