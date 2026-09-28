# BARC-01.2 client-mode result transport decision

Status: BARC-01.2a implemented and locally qualified; native runtime
qualification remains pending. The native asset smoke proof remains in
`barc-01-native-asset-proof.md`. The client-mode result path is a separate
cross-repository contract change, so it is not an independent narrow source
patch for this staging PR.

## Existing paths and exact gap

- `proto/highbar/service.proto` defines `CommandAck.results` as repeated
  `CommandBatchResult`. The plugin-hosted `HighBarService.cpp` fills it in its
  own `SubmitCommands` handler.
- Client mode uses `proto/highbar/coordinator.proto` instead.
  `OpenCommandChannel` streams `CommandBatch` from coordinator to plugin but
  has no plugin-to-coordinator result message. `CoordinatorClient.cpp` admits
  each batch to the native queue and logs its sequence, correlation ID, and
  status locally. The Python example coordinator returns aggregate counters
  as soon as it has forwarded the batches. The live result was
  `accepted=1 results=0` even though the engine moved the commanded unit.
- Dispatch is a later, distinct outcome. `GrpcGatewayModule.cpp` writes
  `CommandDispatchEvent` with batch sequence, client command ID, command
  index, target, frame, and status into `StateDelta`; `PushState` already
  carries that event to the coordinator. A client can correlate a dispatch
  event from `StreamState`, but this does not supply the missing native
  admission result in `CommandAck`.
- FSBarV2 also implements `OpenCommandChannel` in
  `Broker.Protocol/HighBarCoordinatorService.fs`. A new coordinator RPC or
  envelope therefore needs matching FSBarV2 generated contract and service
  handling, not just the Python example.

## Smallest viable contract change to design next

Preserve `OpenCommandChannel` and add a plugin-to-coordinator admission-result
RPC carrying `plugin_id` plus a `CommandBatchResult`. The coordinator must
register each pending `(plugin_id, batch_seq, client_command_id)` before
forwarding, accept only a matching result from the owning plugin, and complete
the submitter's `CommandAck.results` after native admission. A timeout or
disconnect must be reported as an unknown/unavailable outcome, never as
native acceptance. Late or duplicate results must not complete another
submitter. The result has to be sent after `AdmitCommandBatch`, outside the
engine thread, with bounded backpressure and an explicit retry/idempotency
policy. Dispatch results remain on the existing state stream and must be
matched separately by the same batch and client command IDs.

This requires deciding the wire method, result persistence across reconnect,
how a streamed `SubmitCommands` call waits for all per-batch results, and how
multiple plugin sessions are routed. Today's Python relay has one shared
command queue and only an active-channel count, which does not identify the
plugin that took a given batch. Adding an RPC without those rules could
attribute a result to the wrong submitter or falsely report accepted work.

## Frozen BARC-01.2a producer contract

The additive producer contract is `ReportCommandBatchResult`, a bounded unary
RPC made by the plugin command-reader thread after atomic native admission.
`CommandChannelSubscribe` explicitly negotiates
`ADMISSION_RESULT_PROTOCOL_CORRELATED_V1` and carries a fresh
`channel_incarnation`. The report repeats the owning plugin, incarnation,
strict base schema version, full `uint64` batch/correlation identity, and the
existing `CommandBatchResult`. A report acknowledgement distinguishes
recorded, duplicate, and late results.

The example coordinator permits one owning plugin channel, registers pending
work before forwarding, rejects colliding identities, and keys each waiter by
incarnation, batch sequence, and correlation. Wrong-owner and old-incarnation
reports fail; duplicate reports are idempotent; late reports cannot satisfy a
new waiter. Timeout, cancellation, EOF, and reconnect after forwarding produce
an unknown terminal RPC outcome and never retry the command. Explicit
`legacy-observation-only` mode forwards for compatibility while returning no
native results or accepted counters.

Queue entries and `CommandDispatchEvent` now retain the channel incarnation.
Admission remains separate from later engine dispatch and observed state
change. The exact frozen source hashes are recorded in the delivery commit and
qualification report; downstream receivers must pin that commit rather than a
mutable branch.

## Compatibility and verification gates

- Negotiate result support explicitly. An old coordinator will not implement
  the new RPC; an old plugin will never send results. Either pair must remain
  aggregate-only with that limitation visible to callers, or reject a client
  that explicitly requires native results. Do not fill `results` speculatively
  from the coordinator's forwarding count. Update the pinned schema/version
  policy and generated C++, Python, and F# bindings together.
- Native tests: accepted, invalid, and queue-full admission; exact 64-bit
  correlation preservation; duplicate/late result, timeout, reconnect, and
  mismatched plugin identity. Coordinator tests: concurrent submitters and
  plugins, stream EOF, cancellation, and one result per batch with no
  cross-session attribution. FSBarV2 tests must exercise the same contract.
- Live headless check: submit one valid and one rejected batch against the
  exact pinned engine/game/map/plugin, assert their structured ACK statuses
  and IDs, then assert the valid command's correlated
  `CommandDispatchEvent` and observed engine effect. Old/new pairing tests
  must verify the declared fallback or rejection behavior.

The source and test surface spans protobuf, generated bindings, native
command-reader lifecycle, coordinator relay state, FSBarV2, and live headless
qualification. It is dependent BARC-01.2 work rather than a narrow follow-on
that can be admitted independently of the current native proof staging PR.
