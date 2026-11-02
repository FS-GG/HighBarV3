# Building HighBarV3

End-to-end runbook for taking a clean checkout to a live
`spring-headless` match with the gRPC gateway running and an external
Python observer attached. Each numbered step has one bash block and a
`<!-- expect: <substring> -->` comment that
`tests/headless/build-runbook-validation.sh` checks against the block's
stdout+stderr.

The current spring-mode bind has a known issue inside spring-headless
documented in
[`specs/002-live-headless-e2e/investigations/hello-rpc-deadline-exceeded.md`](specs/002-live-headless-e2e/investigations/hello-rpc-deadline-exceeded.md);
this runbook uses **client-mode** (plugin dials a coordinator) which
sidesteps it. The reference scripts live at
[`specs/002-live-headless-e2e/examples/`](specs/002-live-headless-e2e/examples/).

## Prerequisites

- Linux x86_64, Ubuntu 22.04 (or Arch with the equivalent package set).
- `git`, `curl`, `zip`, `unzip`, `tar`, `cmake ≥ 3.27`, `ninja`,
  `g++ ≥ 13`, `python3 ≥ 3.11`, `buf`, `pkg-config`, `protoc`.
- `spring-headless` installed at
  `~/.local/state/Beyond All Reason/engine/recoil_2025.06.19/spring-headless`
  (matches `data/config/spring-headless.pin`).
- `~/vcpkg` cloned at baseline `80aae04504a834399456d74605204bfe514762bf`,
  bootstrapped (`./bootstrap-vcpkg.sh -disableMetrics`).
- The BAR Recoil engine source cloned at
  `~/recoil-engine` (tag `2025.06.19`, with submodules
  `rts/lib/{tracy,gflags,entt,cereal,fastgltf,simdjson,RmlUi,lunasvg}`,
  `tools/pr-downloader` initialized) and our HighBarV3 working tree
  symlinked in as `~/recoil-engine/AI/Skirmish/BARb`.

## 1. Pre-flight: the engine pin matches what's installed

<!-- expect: e4f63c1a391f9ddfbb4d1da225d9533b1d56c65133687d036422a7380c84e833 -->
```bash
sha256sum "$HOME/.local/state/Beyond All Reason/engine/recoil_2025.06.19/spring-headless" | cut -d' ' -f1
```

## 2. Install gRPC + Protobuf via vcpkg manifest mode

<!-- expect: All requested installations completed successfully -->
```bash
~/vcpkg/vcpkg install --triplet=x64-linux 2>&1 | tail -3
```

## 3. Generate proto stubs (C++ + Python)

<!-- expect: build/gen/highbar/service.grpc.pb.h -->
```bash
~/vcpkg/installed/x64-linux/tools/protobuf/protoc \
    -I proto \
    --cpp_out=build/gen \
    --grpc_out=build/gen \
    --plugin=protoc-gen-grpc=$HOME/vcpkg/installed/x64-linux/tools/grpc/grpc_cpp_plugin \
    proto/highbar/*.proto
ls build/gen/highbar/service.grpc.pb.h
```

## Standalone CMake Smoke Tests

The repository root can be configured directly for proto-backed tests that do
not need Recoil engine headers:

```bash
cmake -S . -B build
cmake --build build --target command_queue_test -j 4
ctest --test-dir build --output-on-failure -R command_queue_test
```

The full plugin, validator tests, and engine-thread integration tests still
require the engine-tree configuration in step 4 because they include Recoil
headers and use `configure_native_skirmish_ai`.

## 4. Configure the BAR engine + AI subtree (AI-only build)

<!-- expect: Build files have been written to: /home/ -->
```bash
VCPKG_INSTALLED=$PWD/vcpkg_installed/x64-linux
cd ~/recoil-engine && CXXFLAGS=-fuse-ld=gold CFLAGS=-fuse-ld=gold LDFLAGS=-fuse-ld=gold \
    cmake -S . -B build -G Ninja \
    -DAI_TYPES=NATIVE -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DHIGHBAR_AI_ONLY=ON \
    -DCMAKE_EXE_LINKER_FLAGS_INIT=-fuse-ld=gold \
    -DCMAKE_SHARED_LINKER_FLAGS_INIT=-fuse-ld=gold \
    -DCMAKE_MODULE_LINKER_FLAGS_INIT=-fuse-ld=gold \
    -DCMAKE_PREFIX_PATH=$VCPKG_INSTALLED \
    -DgRPC_DIR=$VCPKG_INSTALLED/share/grpc \
    -DProtobuf_DIR=$VCPKG_INSTALLED/share/protobuf \
    -Dabsl_DIR=$VCPKG_INSTALLED/share/absl \
    -Dutf8_range_DIR=$VCPKG_INSTALLED/share/utf8_range \
    -Dre2_DIR=$VCPKG_INSTALLED/share/re2 \
    -Dc-ares_DIR=$VCPKG_INSTALLED/share/c-ares 2>&1 | tail -1
```

## 5. Build `libSkirmishAI.so`

<!-- expect: Linking CXX shared module AI/Skirmish/BARb/data/libSkirmishAI.so -->
```bash
cmake --build ~/recoil-engine/build --target BARb -j 8 2>&1 | tail -3
```

## 6. Run the AICommand arm-coverage check (66/66 wired)

<!-- expect: arm-coverage CSV written -->
```bash
tests/headless/_gen-arm-coverage.sh --repo-root . --output build/reports/aicommand-arm-coverage.csv
echo "wired: $(grep -c ',true,' build/reports/aicommand-arm-coverage.csv) / $(grep -cv arm_name build/reports/aicommand-arm-coverage.csv)"
```

## 7. Install the built `.so` into a separate `highBar` AI dir

<!-- expect: HighBarV3 install OK -->
```bash
BAR_DIR="$HOME/.local/state/Beyond All Reason/engine/recoil_2025.06.19"
HIGHBAR_DIR="$BAR_DIR/AI/Skirmish/highBar/stable"
mkdir -p "$HIGHBAR_DIR/config/dev" "$HIGHBAR_DIR/script/dev"
cp data/AIInfo.lua "$HIGHBAR_DIR/AIInfo.lua"
cp data/AIOptions.lua "$HIGHBAR_DIR/AIOptions.lua"
cp data/config/*.json "$HIGHBAR_DIR/config/"
cp data/config/dev/*.json "$HIGHBAR_DIR/config/dev/"
cp data/script/*.as "$HIGHBAR_DIR/script/"
cp data/script/dev/*.as "$HIGHBAR_DIR/script/dev/"
cp ~/recoil-engine/build/AI/Skirmish/BARb/data/libSkirmishAI.so \
   "$HIGHBAR_DIR/libSkirmishAI.so"
echo "HighBarV3 install OK"
```

## 8. Start the coordinator (HighBarCoordinator + HighBarProxy services)

<!-- expect: coordinator id=coord listening -->
```bash
mkdir -p /tmp/hb-run
rm -f /tmp/hb-run/hb-coord.sock
python3 -m grpc_tools.protoc -I proto --python_out=/tmp/hb-run/pyproto --grpc_python_out=/tmp/hb-run/pyproto proto/highbar/*.proto
python3 specs/002-live-headless-e2e/examples/coordinator.py \
    --endpoint unix:/tmp/hb-run/hb-coord.sock --id coord \
    > /tmp/hb-run/coord.log 2>&1 &
echo $! > /tmp/hb-run/coord.pid
sleep 1
head -1 /tmp/hb-run/coord.log
```

## 9. Launch `spring-headless` with the plugin dialing the coordinator

<!-- expect: [hb-gateway] startup transport=uds bind= -->
```bash
BAR_DIR="$HOME/.local/state/Beyond All Reason"
export SPRING_DATADIR="$BAR_DIR"
export XDG_RUNTIME_DIR=/tmp/hb-run
export HIGHBAR_COORDINATOR=unix:/tmp/hb-run/hb-coord.sock
"$BAR_DIR/engine/recoil_2025.06.19/spring-headless" \
    /tmp/hb-run/match.startscript > /tmp/hb-run/match.log 2>&1 &
echo $! > /tmp/hb-run/spring.pid
# Wait for the gateway to start (startup banner appears within ~10s).
for i in $(seq 1 15); do
    if grep -q '\[hb-gateway\] startup' /tmp/hb-run/match.log 2>/dev/null; then break; fi
    sleep 1
done
grep '\[hb-gateway\] startup' /tmp/hb-run/match.log
```

## 10. Attach the Python observer and confirm a `StateUpdate` arrives

<!-- expect: [obs rx=00001] -->
```bash
sleep 3
python3 specs/002-live-headless-e2e/examples/observer.py \
    --endpoint unix:/tmp/hb-run/hb-coord.sock --max 1 2>&1 | tee /tmp/hb-run/obs.log
# clean up
kill -TERM $(cat /tmp/hb-run/spring.pid) 2>/dev/null
kill -TERM $(cat /tmp/hb-run/coord.pid) 2>/dev/null
```

## Troubleshooting

- **Step 4 fails on `Tracy::TracyClient` not found**: re-init the
  `rts/lib/tracy` submodule.
- **Step 5 `EnemyInfo.h: No such file`**: ensure the V3 `EnemyInfo.h`
  shim under `src/circuit/unit/enemy/` is present (it forwards to
  `EnemyUnit.h` where `CEnemyInfo` is actually declared).
- **Step 9 hangs at `f=-000001`**: `IModule::InitScript()` is
  dereferencing a null `script`. Verify
  `CGrpcGatewayModule::InitScript()` is overridden to return `true` and
  `IModule::InitScript()` is `virtual` in `Module.h`.
- **Step 10 hangs / `DEADLINE_EXCEEDED` against the plugin's UDS
  socket**: that's the gRPC server-mode bug. The runbook above uses
  client-mode (plugin dials the coordinator) — connect the observer
  to the coordinator endpoint, not to `/tmp/hb-run/highbar-0.sock`.
