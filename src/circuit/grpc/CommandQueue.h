// SPDX-License-Identifier: GPL-2.0-only
//
// HighBarV3 — MPSC bounded command queue (T055).
//
// Multiple gRPC worker threads push accepted AICommands here; the
// engine thread drains the queue at the top of every frame tick
// (T057). Overflow returns synchronously so the wire side can reply
// RESOURCE_EXHAUSTED without ever dropping or reordering commands
// already in the queue (FR-012a, data-model §4).

#pragma once

#include "highbar/commands.pb.h"
#include "highbar/live_control.pb.h"

#include <cstdint>
#include <chrono>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

namespace circuit::grpc {

class Counters;

// Single entry: the command to dispatch plus the originating session
// for logging / per-session accounting. Commands are shallow-copied
// onto the queue so the caller's batch message can be freed once
// Push returns.
struct QueuedCommand {
	std::string session_id;
	std::string channel_incarnation;
	std::uint64_t batch_seq = 0;
	std::uint64_t client_command_id = 0;
	std::uint32_t command_index = 0;
	std::int32_t authoritative_target_unit_id = 0;
	::highbar::v1::AICommand command;
	bool live = false;
	::highbar::v1::LiveBinding live_binding;
	::highbar::v1::NativeObservationBasis live_basis;
	::highbar::v1::NativeUnitReference live_actor;
	std::optional<::highbar::v1::NativeUnitReference> live_attack_target;
	std::optional<::highbar::v1::NativeTacticalCommand> live_tactical_command;
	::highbar::v1::LiveSemanticAction live_semantic_action =
		::highbar::v1::LIVE_SEMANTIC_ACTION_UNSPECIFIED;
	std::chrono::steady_clock::time_point live_basis_deadline{};
	std::chrono::steady_clock::time_point live_command_deadline{};
	std::chrono::steady_clock::time_point live_lease_deadline{};
};

enum class CommandBatchAdmissionStatus {
	kAccepted,
	kInvalidEmpty,
	kInvalidOversized,
	kInvalidTarget,
	kInvalidBatchSequence,
	kInvalidCorrelation,
	kDuplicate,
	kQueueFull,
};

// Engine-independent result for coordinator command admission. This is
// intentionally local to the native queue boundary; it is not the protobuf
// acknowledgement type used by HighBarService.
struct CommandBatchResult {
	CommandBatchAdmissionStatus status =
		CommandBatchAdmissionStatus::kInvalidEmpty;
	std::size_t accepted_command_count = 0;

	bool accepted() const {
		return status == CommandBatchAdmissionStatus::kAccepted;
	}
};

class CommandQueue {
public:
	// `counters` may be null for unit tests. `capacity` is the bounded
	// depth; defaults to 1024 per tasks.md T055.
	explicit CommandQueue(Counters* counters = nullptr,
	                      std::size_t capacity = 1024);

	// Attempt to enqueue. Returns true on success. On failure the queue
	// is already at capacity — the caller must report RESOURCE_EXHAUSTED
	// to the client without mutating state. Already-queued commands are
	// never dropped or reordered.
	bool TryPush(QueuedCommand cmd);

	// Atomic batch enqueue. Returns false when the entire batch cannot
	// fit; in that case no command from `cmds` is pushed.
	bool TryPushBatch(std::vector<QueuedCommand> cmds);

	std::size_t AvailableCapacity() const;

	// Engine-thread drain. Moves up to `max` entries into `out` and
	// returns the number actually moved. Called from OnFrameTick at the
	// top of every frame so throughput is bounded by engine frame rate.
	// Pass 0 to drain everything.
	std::size_t Drain(std::vector<QueuedCommand>* out,
	                  std::size_t max = 0);

	// Current depth. Cheap but approximate under concurrent access —
	// reflected into Counters::command_queue_depth atomically on every
	// push / drain.
	std::size_t Depth() const;

	std::size_t Capacity() const { return capacity_; }

private:
	Counters* counters_;
	const std::size_t capacity_;
	mutable std::mutex mutex_;
	std::queue<QueuedCommand> queue_;
};

// Validate and atomically admit one coordinator batch. The helper preserves
// the coordinator's complete provenance on every queued child and calls
// TryPushBatch exactly once after all validation and construction succeed.
CommandBatchResult AdmitCommandBatch(
	CommandQueue& queue,
	const ::highbar::v1::CommandBatch& batch,
	const std::string& session_id,
	const std::string& channel_incarnation = {});

class LiveControlState;
CommandBatchResult AdmitLiveCommandBatch(
	CommandQueue& queue,
	const ::highbar::v1::LiveCommandBatch& live,
	const std::string& session_id,
	LiveControlState& state,
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

}  // namespace circuit::grpc
