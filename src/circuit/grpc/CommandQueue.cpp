// SPDX-License-Identifier: GPL-2.0-only
//
// HighBarV3 — CommandQueue impl (T055).

#include "grpc/CommandQueue.h"
#include "grpc/Counters.h"
#include "grpc/LiveControlState.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace circuit::grpc {

CommandQueue::CommandQueue(Counters* counters, std::size_t capacity)
	: counters_(counters), capacity_(capacity) {}

bool CommandQueue::TryPush(QueuedCommand cmd) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (queue_.size() >= capacity_) {
		return false;
	}
	queue_.push(std::move(cmd));
	if (counters_ != nullptr) {
		counters_->command_queue_depth.store(
			static_cast<std::uint32_t>(queue_.size()),
			std::memory_order_relaxed);
	}
	return true;
}

bool CommandQueue::TryPushBatch(std::vector<QueuedCommand> cmds) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (cmds.size() > capacity_ - queue_.size()) {
		return false;
	}
	for (auto& cmd : cmds) {
		queue_.push(std::move(cmd));
	}
	if (counters_ != nullptr) {
		counters_->command_queue_depth.store(
			static_cast<std::uint32_t>(queue_.size()),
			std::memory_order_relaxed);
	}
	return true;
}

std::size_t CommandQueue::Drain(std::vector<QueuedCommand>* out,
                                std::size_t max) {
	if (out == nullptr) return 0;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::size_t budget = (max == 0) ? queue_.size()
	                                      : std::min(max, queue_.size());
	out->reserve(out->size() + budget);
	for (std::size_t i = 0; i < budget; ++i) {
		out->push_back(std::move(queue_.front()));
		queue_.pop();
	}
	if (counters_ != nullptr) {
		counters_->command_queue_depth.store(
			static_cast<std::uint32_t>(queue_.size()),
			std::memory_order_relaxed);
	}
	return budget;
}

std::size_t CommandQueue::Depth() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return queue_.size();
}

std::size_t CommandQueue::AvailableCapacity() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return capacity_ - queue_.size();
}

CommandBatchResult AdmitCommandBatch(
		CommandQueue& queue,
		const ::highbar::v1::CommandBatch& batch,
		const std::string& session_id,
		const std::string& channel_incarnation) {
	constexpr int kMaxCoordinatorBatchCommands = 64;
	const int command_count = batch.commands_size();
	if (command_count == 0) {
		return {CommandBatchAdmissionStatus::kInvalidEmpty, 0};
	}
	if (command_count > kMaxCoordinatorBatchCommands) {
		return {CommandBatchAdmissionStatus::kInvalidOversized, 0};
	}
	if (batch.target_unit_id()
	    > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
		return {CommandBatchAdmissionStatus::kInvalidTarget, 0};
	}
	if (batch.batch_seq() == 0) {
		return {CommandBatchAdmissionStatus::kInvalidBatchSequence, 0};
	}
	if (!batch.has_client_command_id() || batch.client_command_id() == 0) {
		return {CommandBatchAdmissionStatus::kInvalidCorrelation, 0};
	}

	std::vector<QueuedCommand> queued;
	queued.reserve(static_cast<std::size_t>(command_count));
	for (int i = 0; i < command_count; ++i) {
		QueuedCommand child;
		child.session_id = session_id;
		child.channel_incarnation = channel_incarnation;
		child.batch_seq = batch.batch_seq();
		child.client_command_id = batch.client_command_id();
		child.command_index = static_cast<std::uint32_t>(i);
		child.authoritative_target_unit_id =
			static_cast<std::int32_t>(batch.target_unit_id());
		child.command = batch.commands(i);
		queued.push_back(std::move(child));
	}

	if (!queue.TryPushBatch(std::move(queued))) {
		return {CommandBatchAdmissionStatus::kQueueFull, 0};
	}
	return {CommandBatchAdmissionStatus::kAccepted,
	        static_cast<std::size_t>(command_count)};
}

CommandBatchResult AdmitLiveCommandBatch(
		CommandQueue& queue, const ::highbar::v1::LiveCommandBatch& live,
		const std::string& session_id, LiveControlState& state,
		std::chrono::steady_clock::time_point now) {
	const auto& batch = live.batch();
	if (batch.commands_size() != 1) return {CommandBatchAdmissionStatus::kInvalidOversized, 0};
	if (!live.has_actor() || live.actor().lifetime() == 0 || live.actor().id() > 31999u)
		return {CommandBatchAdmissionStatus::kInvalidTarget, 0};
	if (batch.target_unit_id() != live.actor().id())
		return {CommandBatchAdmissionStatus::kInvalidTarget, 0};
	if (!state.CheckAuthority(live.binding(), now).ok || !state.BasisKnown(live.basis()))
		return {CommandBatchAdmissionStatus::kInvalidTarget, 0};
	if (live.remaining_basis_validity_ms() == 0
	    || live.remaining_command_lifetime_ms() == 0
	    || live.remaining_lease_validity_ms() == 0)
		return {CommandBatchAdmissionStatus::kInvalidTarget, 0};
	const auto& cmd = batch.commands(0);
	bool semantic_ok = false;
	switch (live.semantic_action()) {
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_STOP:
		semantic_ok = cmd.command_case() == ::highbar::v1::AICommand::kStop
			&& cmd.stop().options() == 0 && cmd.stop().unit_id() >= 0
			&& static_cast<std::uint32_t>(cmd.stop().unit_id()) == live.actor().id();
		break;
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_MOVE_REPLACE:
		semantic_ok = cmd.command_case() == ::highbar::v1::AICommand::kMoveUnit
			&& cmd.move_unit().options() == 0 && cmd.move_unit().unit_id() >= 0
			&& static_cast<std::uint32_t>(cmd.move_unit().unit_id()) == live.actor().id();
		break;
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_MOVE_APPEND:
		semantic_ok = cmd.command_case() == ::highbar::v1::AICommand::kMoveUnit
			&& cmd.move_unit().options() == 32u && cmd.move_unit().unit_id() >= 0
			&& static_cast<std::uint32_t>(cmd.move_unit().unit_id()) == live.actor().id();
		break;
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_ATTACK_VISIBLE_UNIT:
		semantic_ok = cmd.command_case() == ::highbar::v1::AICommand::kAttack
			&& cmd.attack().options() == 0 && live.has_visible_attack_target()
			&& cmd.attack().unit_id() >= 0
			&& static_cast<std::uint32_t>(cmd.attack().unit_id()) == live.actor().id()
			&& live.visible_attack_target().lifetime() != 0
			&& live.visible_attack_target().id() <= 31999u
			&& cmd.attack().target_unit_id() >= 0
			&& static_cast<std::uint32_t>(cmd.attack().target_unit_id()) == live.visible_attack_target().id();
		break;
	default: break;
	}
	if (!semantic_ok) return {CommandBatchAdmissionStatus::kInvalidTarget, 0};
	if (batch.batch_seq() == 0)
		return {CommandBatchAdmissionStatus::kInvalidBatchSequence, 0};
	if (!batch.has_client_command_id() || batch.client_command_id() == 0)
		return {CommandBatchAdmissionStatus::kInvalidCorrelation, 0};
	QueuedCommand q;
	q.session_id = session_id; q.channel_incarnation = live.binding().command_channel_incarnation();
	q.batch_seq = batch.batch_seq(); q.client_command_id = batch.client_command_id(); q.command_index = 0;
	q.authoritative_target_unit_id = static_cast<std::int32_t>(live.actor().id()); q.command = cmd;
	q.live = true; q.live_binding = live.binding(); q.live_basis = live.basis(); q.live_actor = live.actor();
	if (live.has_visible_attack_target()) q.live_attack_target = live.visible_attack_target();
	q.live_semantic_action = live.semantic_action();
	q.live_basis_deadline = now + std::chrono::milliseconds(live.remaining_basis_validity_ms());
	q.live_command_deadline = now + std::chrono::milliseconds(live.remaining_command_lifetime_ms());
	q.live_lease_deadline = now + std::chrono::milliseconds(live.remaining_lease_validity_ms());
	std::vector<QueuedCommand> one; one.push_back(std::move(q));
	if (!queue.TryPushBatch(std::move(one))) return {CommandBatchAdmissionStatus::kQueueFull, 0};
	return {CommandBatchAdmissionStatus::kAccepted, 1};
}

}  // namespace circuit::grpc
