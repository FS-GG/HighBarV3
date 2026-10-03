# BARC-01 Coordinator Admission

## BARC-01.1f — Native coordinator batch admission

- [x] Validate coordinator batches before queue mutation: 1–64 commands,
  signed-engine-representable target, and nonzero 64-bit sequence and correlation.
- [x] Preserve session, batch sequence, client correlation, command index, and
  authoritative target on every queued child.
- [x] Admit a complete coordinator batch with one atomic `TryPushBatch` call.
- [x] Report typed accepted, invalid, and queue-full outcomes to coordinator
  counters and trace/error logs.
- [x] Cover exact three-child provenance, values above `UInt32`, atomic overflow,
  invalid and oversized batches, and concurrent non-interleaving.

Validation on 2026-09-28 from protected `master` `66483515`:

- Standalone native configure and compilation succeeded with CMake 4.4.3,
  GCC 16.2.1, protobuf 36.1, gRPC 1.84.0, and GTest 1.18.0.
- `command_queue_test` and `coordinator_command_admission_test` both passed.
- `coordinator_command_admission_test` passed 100 consecutive executions.
- `CoordinatorClient.cpp` passed a standalone C++20 syntax compile against the
  generated protobuf/gRPC headers.

## BARC-01.5f — bounded owned-damage replacement source window

2026-10-03 source follow-up, retaining the FSBar-owned BARC-01.5f horizon.
Owned damage now requests the existing coalesced engine-thread full entity/economy
replacement. Only handled sparse owned/enemy damage and enemy destruction arms
are removed from the coordinator projection when that replacement is scheduled;
legacy bytes, dispatch/economy order and unsupported lifecycle/unknown arms remain
intact. Resulting owned health still comes from the engine wrapper in
`SnapshotBuilder::FillOwnUnits`; no damage subtraction or protocol change occurs.

The production planner/projector and snapshot publication gate have focused
compiled coverage: 128 damage notifications coalesce to one replacement; mixed
dispatch/economy retain order; sparse-only sequence 35 is skipped and complete
replacement 36 can recover; deferred scheduling retains the replacement request;
failed build/serialization or enqueue cannot grant a new basis; recorded identity
is the exact enqueued snapshot identity. Controlled engine observations serialize
owned unit 42 with health 83 through the actual protobuf implementation. The
optional `HIGHBAR_CONTROLLED_FIXTURE_DIR` output from
`state_update_projection_test` emits legacy/projected/replacement `.pb` files for
the subsequent actual FSBar typed-reducer join. These are **controlled fixtures**,
not reconstructed native payload evidence: the retained native packet lacks raw
StateDelta bytes. This test does not execute the engine manager walk.

Qualification: focused C++ test compilation/execution and gateway syntax compile
against retained Recoil 2026.07.04 engine/SDK headers. Full ABI-matched plugin
rebuild, source/build/configuration/loaded custody, consumer metadata ordering and
native operation remain separate integrator gates. No publication or native
operation is granted; native useful play remains **0/6**. Creation, finish,
ownership, visibility entry and owned removal remain outside this source window.
Telemetry begin returned `not-configured`; native usage remains **Unknown**.
The exact-schema focused build ran **14 passing tests**; removing owned-damage
projection from a private header copy reproduced both sparse-only and burst test
failures. The gateway syntax check passed in 5.05 seconds (existing `LOG` macro
redefinition warning); the focused exact-schema compilation took 12.35 seconds
with one compiler process. Neither check rebuilt or loaded a native plugin.
