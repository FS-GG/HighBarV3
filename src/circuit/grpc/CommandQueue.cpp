// SPDX-License-Identifier: GPL-2.0-only
//
// HighBarV3 — CommandQueue impl (T055).

#include "grpc/CommandQueue.h"
#include "grpc/Counters.h"

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

}  // namespace circuit::grpc
