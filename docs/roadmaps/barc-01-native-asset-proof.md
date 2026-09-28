# BARC-01 native asset and correlated result proof

This records a reproducible local asset set and the limit of native evidence as
of 2026-09-28. It proves one live MoveUnit engine effect but does not qualify
the full BARC-01.2 command/result contract.

## BARC-01.2a-b correlated native qualification

HighBar source commit `dd6f5ef909905a8b5182d3d52c10946527ea79e5`
froze the additive result RPC. The exact coordinator proto SHA-256 is
`b8d3f56494564a8628a20ffdcc2ac7e42a0508f8f1162bcc87ee6c0f6b7c3c1d`,
the state proto SHA-256 is
`11ff63ac8211cbb6530e9be3ce4323a8b5911306d85c1b472c6aaa4618fc78d8`,
and the ordered `proto/highbar/*.proto` hash manifest hashes to
`796278dda7e178a5592413e9842795711efae7b101bc502d1501622bfb5c316e`.

A fresh Recoil build tree (`build333`) compiled the generated RPC, native
client, queue provenance, and dispatch event. Its candidate and installed
`libSkirmishAI.so` both hash to
`5407312b3c9e3a8a7c10db79473aa57c65a03b6767eba2567bc00ca7be9f4cbd`.
The focused native queue tests and five Python transport tests passed. Those
tests cover full `uint64` identity, registration before forwarding, immediate
report races, concurrent submitters, second-owner rejection, wrong and old
incarnations, duplicate and late reports, disconnect, timeout, and explicit
legacy observation-only behavior.

The fresh runtime used `HIGHBAR_COORDINATOR_OWNER_SKIRMISH_AI_ID=1`. A valid
MoveUnit used batch 1 and correlation `9223372036854775825`. Its real
`CommandAck` contained one `COMMAND_BATCH_ACCEPTED` native result with that
exact identity. The state stream then carried a matching
`COMMAND_DISPATCH_APPLIED` event with channel incarnation
`highbar-e4f63c1a391f-215328174065538-1`, and commander 25947 moved from
`(500.0, 349.8, 397.0)` to `(990.4, 352.0, 397.3)`, a 490.4-elmo effect.

A second MoveUnit used batch 2 with an absent correlation. It passed the
coordinator's syntax check and reached native admission, which returned one
`COMMAND_BATCH_REJECTED_INVALID` result with accepted count zero and exact
correlation zero. No batch-2 dispatch event appeared and the plugin trace
showed only the valid batch entering dispatch. This separates native
admission, engine dispatch, and observed state change.

The retained raw log hashes are:

| Log | SHA-256 |
| --- | --- |
| `behavioral-move.log` | `dc61dd9798f34d2d77885665df8e264ef4884a41c7e814ede87d3967c7914257` |
| `coord.log` | `e7cc300e03dff0b238eaaabcd735b5e245f11380bcea96a8b1e95752e2fbad6a` |
| `coordinator-trace.log` | `c7b2b84f77ba7039016d28fc5cde75a25972557992cd39c1978309947bda9219` |
| `highbar-launch.log` | `c57af7e4424fce8a71c7a6373c8febc3575648a225b2867d3b5d524d044231cf` |

They remain under `/tmp/barc-native-result-run3-20260928/`. This completes
the HighBar BARC-01.2a-b producer and native proof window. The FSBar receiver
and its through-broker proof are the separate BARC-01.2c boundary.

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
