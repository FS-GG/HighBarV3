# BARC-01 native asset and smoke proof (partial)

This records a reproducible local asset set and the limit of native evidence as
of 2026-09-28. It proves one live MoveUnit engine effect but does not qualify
the full BARC-01.2 command/result contract.

## Exact assets

| Asset | Primary acquisition route | SHA-256 |
| --- | --- | --- |
| Recoil 2025.06.19 archive | `https://github.com/beyond-all-reason/RecoilEngine/releases/download/2025.06.19/recoil_2025.06.19_amd64-linux.7z` | `5a81dc7bc20bba6ba0f77941470c95f882553b8e377aace1a1b047cb9629cb7f` |
| `spring-headless` inside archive | Extract archive | `e4f63c1a391f9ddfbb4d1da225d9533b1d56c65133687d036422a7380c84e833` |
| Avalanche 3.4 | `https://github.com/beyond-all-reason/Maps/releases/download/2.0/avalanche_3.4.sd7` | `3873260c6b5e533490598488eab3ff0b6587c778aee60cbb71186938dfaee426` |
| BAR test game manifest | Engine-bundled `pr-downloader --rapid-download 'Beyond All Reason test-29926-0571aa8'` with `PRD_RAPID_REPO_MASTER=https://repos.beyondallreason.dev/repos.gz` and `PRD_RAPID_USE_STREAMER=false` | `58ca71d252e89e844361293e3b6b0aa2fb29fd217094b83e2a8f51ed1541f250` (`packages/7fd33d9ceb79c052065b8d3c2f6f2687.sdp`) |

`pr-downloader --rapid-validate` and `--validate-sdp` passed. The engine
binary matches `data/config/spring-headless.pin`; the pin's old spring release
URL returned 404, so its acquisition URL now points to the matching official
RecoilEngine release. The RecoilEngine source tag resolves to commit
`2639eedac7d1fd67d793ec93ebd27f014f336a14`.

## Local plugin build

RecoilEngine source at that tag, with this HighBarV3 checkout at
`AI/Skirmish/BARb`, built the `BARb` CMake target after generation of its
protobuf/gRPC C++ stubs. CMake 3.31.10 was installed under `/tmp` because
the host's CMake 4.4.3 rejects old engine policy settings. DevIL, SDL2,
Jasper, 7zip, OpenAL, Ogg, Vorbis, and Xcursor were staged under an isolated
sysroot. No shared system packages were changed. `ldd -r` reported no missing
or unresolved dependencies. The resulting `libSkirmishAI.so` SHA-256 is
`2290b65ae63fdb5f77ed74f0b02973149172be44983c7a8c97c2d4877f826453`.
The C++ `LOG` macro restoration in `CircuitAI.cpp` is needed because Abseil
headers reached through protobuf redefine it after `CircuitAI.h`.

## Bounded live smoke and limit

The built plugin was installed as `AI/Skirmish/highBar/stable/libSkirmishAI.so`
in the isolated engine tree; its installed SHA-256 was checked against the
built artifact. The repository's `minimal.startscript` assigns highBar to AI
ID 1 and NullAI to ID 0, so runs set
`HIGHBAR_COORDINATOR_OWNER_SKIRMISH_AI_ID=1`.

With that setting, `tests/headless/us1-observer.sh` passed: 30 state updates,
last sequence 30, in 9 seconds. The coordinator logged a plugin PushState
stream and OpenCommandChannel subscription. The first
`tests/headless/behavioral-move.sh` attempt stopped at `SubmitCommands`
because the local protobuf runtime's `FieldDescriptor` no longer exposes
`.label` in the Python example coordinator. The example now uses
`.is_repeated`; a direct valid/NaN batch validation probe passed. A second
attempt reached the plugin but its native admission rejected the fixture's
zero client correlation ID. The fixture now sets `client_command_id=1`.

The final bounded move attempt passed. The coordinator logged forwarding
batch sequence 1 with one `move_unit`; the plugin trace logged
`cmd batch admission seq=1 correlation=1 ncmds=1 status=accepted`; and the
engine state snapshots showed commander 25947 moving from `(500.0, 349.8,
397.0)` to `(990.4, 352.0, 397.3)`, a displacement of 490.4 elmos. This
is an observed engine effect tied to the submitted batch. The Python
coordinator still returns only an accepted-batch count from `SubmitCommands`;
no structured `CommandBatchResult` was observed, so end-to-end typed result
correlation and the full BARC-01.2 milestone remain unproven.

A further bounded run printed the actual `CommandAck`: `accepted=1 results=0`
while the same 490.4-elmo engine movement still passed. The reason is in the
client-mode route: `coordinator.proto` defines `OpenCommandChannel` as a
server stream of `CommandBatch` only; `CoordinatorClient.cpp` logs native
admission locally and has no result return RPC. The Python example
coordinator's `SubmitCommands` immediately returns aggregate counters and
never populates `CommandAck.results`. The separate plugin-hosted
`HighBarService.cpp` path does populate structured results, but that is not
the coordinator route used here. A return path for native admission and
dispatch results, with correlation preserved, remains future work.

Local raw logs are under `/tmp/barc-native-assets/us1-run2/`,
`/tmp/barc-native-assets/move-run2/`, and
`/tmp/barc-native-assets/move-run3/`; the accepted batch and engine effect
are in `/tmp/barc-native-assets/coordinator-move-trace3.log` and
`move-run3/behavioral-move.log`. These are ephemeral evidence paths, not
repository fixtures. The BAR test game also emitted Lua feature definition
and headless rendering errors while the engine continued to advance frames.
The confirming aggregate-only ACK is in
`/tmp/barc-native-assets/move-run4/behavioral-move.log`.
