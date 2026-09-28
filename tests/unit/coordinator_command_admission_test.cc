// SPDX-License-Identifier: GPL-2.0-only

#include "grpc/CommandQueue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

using circuit::grpc::AdmitCommandBatch;
using circuit::grpc::CommandBatchAdmissionStatus;
using circuit::grpc::CommandQueue;
using circuit::grpc::QueuedCommand;

::highbar::v1::CommandBatch MakeBatch(
		std::uint64_t sequence,
		std::uint64_t correlation,
		std::uint32_t target,
		std::initializer_list<std::int32_t> markers) {
	::highbar::v1::CommandBatch batch;
	batch.set_batch_seq(sequence);
	batch.set_client_command_id(correlation);
	batch.set_target_unit_id(target);
	for (const auto marker : markers) {
		batch.add_commands()->mutable_move_unit()->set_unit_id(marker);
	}
	return batch;
}

QueuedCommand MakeQueued(std::int32_t marker) {
	QueuedCommand command;
	command.command.mutable_move_unit()->set_unit_id(marker);
	return command;
}

TEST(CoordinatorCommandAdmission, PreservesThreeChildProvenance) {
	CommandQueue queue(nullptr, 8);
	const auto batch = MakeBatch(77, 88, 42, {101, 102, 103});

	const auto result = AdmitCommandBatch(
		queue, batch, "plugin-7-cmd-ch", "incarnation-17");

	ASSERT_TRUE(result.accepted());
	EXPECT_EQ(result.accepted_command_count, 3u);
	std::vector<QueuedCommand> drained;
	ASSERT_EQ(queue.Drain(&drained), 3u);
	for (std::size_t i = 0; i < drained.size(); ++i) {
		EXPECT_EQ(drained[i].session_id, "plugin-7-cmd-ch");
		EXPECT_EQ(drained[i].channel_incarnation, "incarnation-17");
		EXPECT_EQ(drained[i].batch_seq, 77u);
		EXPECT_EQ(drained[i].client_command_id, 88u);
		EXPECT_EQ(drained[i].command_index, i);
		EXPECT_EQ(drained[i].authoritative_target_unit_id, 42);
		EXPECT_EQ(drained[i].command.move_unit().unit_id(),
		          101 + static_cast<std::int32_t>(i));
	}
}

TEST(CoordinatorCommandAdmission, PreservesSequenceAndCorrelationAboveUInt32) {
	constexpr std::uint64_t kSequence =
		static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 17;
	constexpr std::uint64_t kCorrelation = kSequence + 23;
	CommandQueue queue(nullptr, 2);
	const auto batch = MakeBatch(kSequence, kCorrelation, 9, {1});

	ASSERT_TRUE(AdmitCommandBatch(queue, batch, "session").accepted());
	std::vector<QueuedCommand> drained;
	ASSERT_EQ(queue.Drain(&drained), 1u);
	EXPECT_EQ(drained[0].batch_seq, kSequence);
	EXPECT_EQ(drained[0].client_command_id, kCorrelation);
}

TEST(CoordinatorCommandAdmission, OverflowAdmitsZeroAndRetainsExistingOrder) {
	CommandQueue queue(nullptr, 4);
	ASSERT_TRUE(queue.TryPush(MakeQueued(1)));
	ASSERT_TRUE(queue.TryPush(MakeQueued(2)));
	const auto batch = MakeBatch(1, 1, 7, {10, 11, 12});

	const auto result = AdmitCommandBatch(queue, batch, "session");

	EXPECT_EQ(result.status, CommandBatchAdmissionStatus::kQueueFull);
	EXPECT_EQ(result.accepted_command_count, 0u);
	EXPECT_EQ(queue.Depth(), 2u);
	std::vector<QueuedCommand> drained;
	ASSERT_EQ(queue.Drain(&drained), 2u);
	EXPECT_EQ(drained[0].command.move_unit().unit_id(), 1);
	EXPECT_EQ(drained[1].command.move_unit().unit_id(), 2);
}

TEST(CoordinatorCommandAdmission, InvalidAndOversizedBatchesLeaveQueueUnchanged) {
	CommandQueue queue(nullptr, 128);
	ASSERT_TRUE(queue.TryPush(MakeQueued(5)));

	auto empty = MakeBatch(1, 1, 1, {});
	EXPECT_EQ(AdmitCommandBatch(queue, empty, "session").status,
	          CommandBatchAdmissionStatus::kInvalidEmpty);

	auto oversized = MakeBatch(1, 1, 1, {});
	for (int i = 0; i < 65; ++i) {
		oversized.add_commands()->mutable_move_unit()->set_unit_id(i);
	}
	EXPECT_EQ(AdmitCommandBatch(queue, oversized, "session").status,
	          CommandBatchAdmissionStatus::kInvalidOversized);

	const auto first_unrepresentable_target =
		static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) + 1u;
	auto invalid_target = MakeBatch(1, 1, first_unrepresentable_target, {1});
	EXPECT_EQ(AdmitCommandBatch(queue, invalid_target, "session").status,
	          CommandBatchAdmissionStatus::kInvalidTarget);
	EXPECT_EQ(AdmitCommandBatch(queue, MakeBatch(0, 1, 1, {1}), "session").status,
	          CommandBatchAdmissionStatus::kInvalidBatchSequence);
	EXPECT_EQ(AdmitCommandBatch(queue, MakeBatch(1, 0, 1, {1}), "session").status,
	          CommandBatchAdmissionStatus::kInvalidCorrelation);

	auto missing_correlation = MakeBatch(1, 1, 1, {1});
	missing_correlation.clear_client_command_id();
	EXPECT_EQ(AdmitCommandBatch(queue, missing_correlation, "session").status,
	          CommandBatchAdmissionStatus::kInvalidCorrelation);
	EXPECT_EQ(queue.Depth(), 1u);
}

TEST(CoordinatorCommandAdmission, ConcurrentBatchChildrenNeverInterleave) {
	CommandQueue queue(nullptr, 6);
	const auto first = MakeBatch(1, 11, 1, {100, 101, 102});
	const auto second = MakeBatch(2, 22, 2, {200, 201, 202});
	std::atomic<bool> start{false};
	std::atomic<bool> first_accepted{false};
	std::atomic<bool> second_accepted{false};

	std::thread first_thread([&] {
		while (!start.load(std::memory_order_acquire)) {}
		first_accepted.store(
			AdmitCommandBatch(queue, first, "first").accepted(),
			std::memory_order_release);
	});
	std::thread second_thread([&] {
		while (!start.load(std::memory_order_acquire)) {}
		second_accepted.store(
			AdmitCommandBatch(queue, second, "second").accepted(),
			std::memory_order_release);
	});
	start.store(true, std::memory_order_release);
	first_thread.join();
	second_thread.join();

	ASSERT_TRUE(first_accepted.load(std::memory_order_acquire));
	ASSERT_TRUE(second_accepted.load(std::memory_order_acquire));
	std::vector<QueuedCommand> drained;
	ASSERT_EQ(queue.Drain(&drained), 6u);
	const auto first_sequence = drained.front().batch_seq;
	for (std::size_t i = 0; i < 3; ++i) {
		EXPECT_EQ(drained[i].batch_seq, first_sequence);
		EXPECT_EQ(drained[i].command_index, i);
	}
	const auto second_sequence = drained[3].batch_seq;
	EXPECT_NE(first_sequence, second_sequence);
	for (std::size_t i = 3; i < 6; ++i) {
		EXPECT_EQ(drained[i].batch_seq, second_sequence);
		EXPECT_EQ(drained[i].command_index, i - 3);
	}
}

}  // namespace
