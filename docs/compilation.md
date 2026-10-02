# Compiling the Native Plugin

This document explains how to build the HighBarV3 native Skirmish AI
plugin, what files need to be installed into a BAR runtime, and what the
current platform limits are.

HighBarV3 is not a standalone executable. The compiled artifact is a
Spring/Recoil Skirmish AI library named `libSkirmishAI.so`. It must be
built against a matching Recoil engine source tree because it includes
engine AI-wrapper headers and links against the engine's native Skirmish
AI interface.

## Current Support Matrix

| Target | Status |
| --- | --- |
| Linux x86_64 | Supported and locally exercised. |
| Windows | Not currently verified. Requires a separate Windows DLL build. |
| macOS | Not currently verified. |

The Linux artifact in this repo's local build is an ELF shared object:

```text
ELF 64-bit LSB shared object, x86-64, GNU/Linux
```

It is not ABI-compatible with Windows BAR/Recoil. A Windows install
would need a Windows build that produces the correct Skirmish AI DLL for
the same engine/AI interface. Copying the Linux `.so` into a Windows BAR
install will not work.

## Prerequisites

Use the detailed, validated runbook in [`BUILD.md`](../BUILD.md) when
setting up a new machine. The practical requirements are:

- Linux x86_64.
- `git`, `curl`, `zip`, `unzip`, `tar`, `pkg-config`.
- CMake 3.27 or newer and Ninja.
- GCC/G++ 13 or newer.
- Python 3.11 or newer.
- `buf` and `protoc`.
- A vcpkg checkout at the baseline in [`vcpkg.json`](../vcpkg.json).
- vcpkg packages: `grpc`, `protobuf`, and `abseil`.
- BAR/Recoil engine source checked out at the engine version that
  matches [`data/config/spring-headless.pin`](../data/config/spring-headless.pin).
- Engine submodules initialized.

The currently pinned runtime is:

```text
recoil_2026.07.04
```

The reference engine binary path used by the test harness is:

```text
$HOME/.local/state/Beyond All Reason/engine/recoil_2026.07.04/spring-headless
```

## Source Layout

The full plugin build is performed from the Recoil engine tree, with
this repository available as the BARb Skirmish AI subtree:

```text
~/recoil-engine/
`-- AI/
    `-- Skirmish/
        `-- BARb/        -> HighBarV3 checkout or symlink
```

The common local setup is:

```bash
ln -sfn /path/to/HighBarV3 "$HOME/recoil-engine/AI/Skirmish/BARb"
```

If `AI/Skirmish/BARb` already exists, preserve or move it first. Do not
hide a symlink inside the existing directory; the engine build will then
compile the wrong tree.

## Build Steps

Run these commands from the HighBarV3 repository unless the command
explicitly changes directory.

1. Install native dependencies through vcpkg:

```bash
~/vcpkg/vcpkg install --triplet=x64-linux
```

This creates or updates `vcpkg_installed/x64-linux` under the manifest
root. First-time gRPC/protobuf builds can take several minutes.

2. Generate C++ protobuf and gRPC stubs:

```bash
mkdir -p build/gen
~/vcpkg/installed/x64-linux/tools/protobuf/protoc \
  -I proto \
  --cpp_out=build/gen \
  --grpc_out=build/gen \
  --plugin=protoc-gen-grpc="$HOME/vcpkg/installed/x64-linux/tools/grpc/grpc_cpp_plugin" \
  proto/highbar/*.proto
```

The engine-tree CMake build copies these generated files from
`build/gen/highbar` into the engine build directory.

3. Configure the engine for an AI-only build:

```bash
VCPKG_INSTALLED=/path/to/HighBarV3/vcpkg_installed/x64-linux

cd "$HOME/recoil-engine"
CXXFLAGS=-fuse-ld=gold CFLAGS=-fuse-ld=gold LDFLAGS=-fuse-ld=gold \
cmake -S . -B build -G Ninja \
  -DAI_TYPES=NATIVE \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DHIGHBAR_AI_ONLY=ON \
  -DCMAKE_EXE_LINKER_FLAGS_INIT=-fuse-ld=gold \
  -DCMAKE_SHARED_LINKER_FLAGS_INIT=-fuse-ld=gold \
  -DCMAKE_MODULE_LINKER_FLAGS_INIT=-fuse-ld=gold \
  -DCMAKE_PREFIX_PATH="$VCPKG_INSTALLED" \
  -DgRPC_DIR="$VCPKG_INSTALLED/share/grpc" \
  -DProtobuf_DIR="$VCPKG_INSTALLED/share/protobuf" \
  -Dabsl_DIR="$VCPKG_INSTALLED/share/absl" \
  -Dutf8_range_DIR="$VCPKG_INSTALLED/share/utf8_range" \
  -Dre2_DIR="$VCPKG_INSTALLED/share/re2" \
  -Dc-ares_DIR="$VCPKG_INSTALLED/share/c-ares"
```

4. Build the plugin target:

```bash
cmake --build "$HOME/recoil-engine/build" --target BARb -j 8
```

Expected output:

```text
$HOME/recoil-engine/build/AI/Skirmish/BARb/data/libSkirmishAI.so
```

The repo-local headless harness may also use:

```text
build/libSkirmishAI.so
```

depending on how the build was configured. The install steps below only
need the final `libSkirmishAI.so`.

## Optional Smoke Checks

The repository root can build a small standalone CMake subset for tests
that do not need engine headers:

```bash
cmake -S . -B build
cmake --build build --target command_queue_test -j 4
ctest --test-dir build --output-on-failure -R command_queue_test
```

This does not produce the Skirmish AI plugin. It only verifies some
proto-backed support code.

For Python client code and unit tests:

```bash
buf lint proto
cd proto && buf generate
cd ../clients/python
python -m pip install -e '.[dev]'
make codegen
make test
```

## Installing Into BAR

The runtime install directory is:

```text
$BAR_DATA/engine/recoil_2026.07.04/AI/Skirmish/highBar/stable/
```

On the reference Linux setup, `$BAR_DATA` is:

```text
$HOME/.local/state/Beyond All Reason
```

Install these files:

```text
AIInfo.lua
AIOptions.lua
libSkirmishAI.so
config/
script/
```

Example:

```bash
BAR_DATA="$HOME/.local/state/Beyond All Reason"
ENGINE_RELEASE="recoil_2026.07.04"
HIGHBAR_DIR="$BAR_DATA/engine/$ENGINE_RELEASE/AI/Skirmish/highBar/stable"

mkdir -p "$HIGHBAR_DIR/config" "$HIGHBAR_DIR/script"

cp data/AIInfo.lua "$HIGHBAR_DIR/AIInfo.lua"
cp data/AIOptions.lua "$HIGHBAR_DIR/AIOptions.lua"
cp -a data/config/. "$HIGHBAR_DIR/config/"
cp -a data/script/. "$HIGHBAR_DIR/script/"
cp "$HOME/recoil-engine/build/AI/Skirmish/BARb/data/libSkirmishAI.so" \
  "$HIGHBAR_DIR/libSkirmishAI.so"
```

The headless launcher also seeds from the installed BARb config/script
directories when available, then overlays HighBarV3's files. That logic
lives in [`tests/headless/_launch.sh`](../tests/headless/_launch.sh).

For game-side config, the launcher mirrors `config/` and `script/` to:

```text
$BAR_DATA/LuaRules/Configs/highBar/stable/
```

Manual equivalent:

```bash
GAME_CFG_DIR="$BAR_DATA/LuaRules/Configs/highBar/stable"
mkdir -p "$GAME_CFG_DIR/config" "$GAME_CFG_DIR/script"
cp -a data/config/. "$GAME_CFG_DIR/config/"
cp -a data/script/. "$GAME_CFG_DIR/script/"
```

## Artifact Size

Current local sizes:

| Artifact | Size |
| --- | ---: |
| `libSkirmishAI.so` unstripped | about 41 MiB |
| `libSkirmishAI.so` stripped | about 33 MiB |
| stripped `.so.gz` | about 13 MiB |
| `data/config` | about 248 KiB |
| `data/script` | about 96 KiB |
| `AIInfo.lua` + `AIOptions.lua` | about 8 KiB |
| installed AI directory | about 42 MiB |

The binary is intentionally not committed to git. Shared libraries and
the `build/` tree are ignored. If binary distribution is needed, publish
a release asset rather than committing `libSkirmishAI.so`.

## Runtime Configuration

The gateway reads:

```text
config/grpc.json
```

from the installed AI data directory. Important fields include:

- `transport`: `uds` or `tcp`.
- `uds_path`: Unix domain socket path template.
- `tcp_bind`: loopback TCP bind address.
- `ai_token_path`: token file path for AI-role RPCs.
- `snapshot_tick.snapshot_cadence_frames`: snapshot cadence in engine
  frames. Default is `30`, roughly one snapshot per second.
- `snapshot_tick.snapshot_max_units`: unit-count threshold where
  snapshot cadence backs off.

Changing `data/config/grpc.json` in the repository does not affect an
already-installed BAR AI until the file is copied into the installed
`config/` directory again.

## Troubleshooting

If CMake cannot find gRPC or Protobuf, confirm `CMAKE_PREFIX_PATH`,
`gRPC_DIR`, and `Protobuf_DIR` point at the same vcpkg triplet used to
install dependencies.

If the build uses the original upstream BARb tree instead of HighBarV3,
check the `AI/Skirmish/BARb` symlink. A common mistake is creating the
HighBarV3 symlink inside an existing `BARb/` directory.

If `Tracy::TracyClient` or other engine headers are missing, initialize
the Recoil engine submodules. The plugin build depends on engine source,
not only the installed `spring-headless` binary.

If the plugin loads but no gRPC gateway appears, inspect the engine log
for `[hb-gateway]` lines and confirm `config/grpc.json` was installed
next to the AI.

If a Windows BAR install is the target, plan for a separate Windows
port/build pass. The current Linux artifact is not portable across that
ABI boundary.
