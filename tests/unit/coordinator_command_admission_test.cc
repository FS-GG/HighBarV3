// SPDX-License-Identifier: GPL-2.0-only

#include "grpc/CommandQueue.h"
#include "grpc/MixedLifecycleQualification.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
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
using circuit::grpc::MixedLifecycleQualification;

struct PrivateBindingFile {
	std::string path;
	explicit PrivateBindingFile(const std::string& body) {
		char pattern[] = "/tmp/highbar-mixed-lifecycle-XXXXXX";
		const int fd = ::mkstemp(pattern);
		EXPECT_GE(fd, 0);
		path = pattern;
		EXPECT_EQ(::fchmod(fd, 0600), 0);
		EXPECT_EQ(::write(fd, body.data(), body.size()),
		          static_cast<ssize_t>(body.size()));
		EXPECT_EQ(::close(fd), 0);
	}
	~PrivateBindingFile() { if (!path.empty()) std::remove(path.c_str()); }
};

std::string MatchBytes() {
	std::string result;
	for (int i = 0; i < 16; ++i) result.push_back(static_cast<char>(i));
	return result;
}

std::string BindingBody(std::uint32_t actor = 42, std::uint64_t lifetime = 7,
	                    std::uint64_t batch = 11, std::uint64_t client = 13) {
	return "schema=highbar.barc.mixed-lifecycle-fault/v1\n"
		"match_incarnation_hex=000102030405060708090a0b0c0d0e0f\n"
		"actor_id=" + std::to_string(actor) + "\n"
		"actor_lifetime=" + std::to_string(lifetime) + "\n"
		"batch_seq=" + std::to_string(batch) + "\n"
		"client_command_id=" + std::to_string(client) + "\n"
		"nonce=0123456789abcdef0123456789abcdef\n";
}

::highbar::v1::LiveCommandBatch MatchingGuard() {
	::highbar::v1::LiveCommandBatch live;
	live.set_semantic_action(::highbar::v1::LIVE_SEMANTIC_ACTION_GUARD);
	live.mutable_binding()->set_match_incarnation(MatchBytes());
	live.mutable_actor()->set_id(42);
	live.mutable_actor()->set_lifetime(7);
	live.mutable_batch()->set_batch_seq(11);
	live.mutable_batch()->set_client_command_id(13);
	return live;
}

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

TEST(MixedLifecycleQualification, EmptyPathIsInert) {
	MixedLifecycleQualification qualification;
	EXPECT_FALSE(qualification.Enabled());
	EXPECT_FALSE(qualification.PublishAcceptedGuard(MatchingGuard()));
	EXPECT_FALSE(qualification.BeginEngineFrame(9).has_value());
	EXPECT_EQ(qualification.DecisionForFrame(10),
	          MixedLifecycleQualification::Decision::kInactive);
}

TEST(MixedLifecycleQualification, ExactBindingCrossesReaderEngineBoundaryOnce) {
	PrivateBindingFile file(BindingBody());
	MixedLifecycleQualification qualification(file.path);
	auto live = MatchingGuard();
	std::atomic<bool> published{false};
	std::thread reader([&] { published.store(
		qualification.PublishAcceptedGuard(live), std::memory_order_release); });
	reader.join();
	ASSERT_TRUE(published.load(std::memory_order_acquire));
	const auto request = qualification.BeginEngineFrame(100);
	ASSERT_TRUE(request.has_value());
	EXPECT_EQ(request->actor_id, 42u);
	EXPECT_EQ(request->actor_lifetime, 7u);
	EXPECT_EQ(request->nonce, "0123456789abcdef0123456789abcdef");
	EXPECT_TRUE(qualification.Matches(11, 13, 42, 7, MatchBytes()));
	qualification.ObserveDestroyed(42, 8);
	EXPECT_EQ(qualification.DecisionForFrame(100),
	          MixedLifecycleQualification::Decision::kHold);
	qualification.ObserveDestroyed(42, 7);
	EXPECT_EQ(qualification.DecisionForFrame(100),
	          MixedLifecycleQualification::Decision::kRelease);
	EXPECT_FALSE(qualification.PublishAcceptedGuard(live));
}

TEST(MixedLifecycleQualification, MissingLifecycleFailsAfterOneFrame) {
	PrivateBindingFile file(BindingBody());
	MixedLifecycleQualification qualification(file.path);
	ASSERT_TRUE(qualification.PublishAcceptedGuard(MatchingGuard()));
	ASSERT_TRUE(qualification.BeginEngineFrame(50).has_value());
	EXPECT_EQ(qualification.DecisionForFrame(50),
	          MixedLifecycleQualification::Decision::kHold);
	EXPECT_EQ(qualification.DecisionForFrame(51),
	          MixedLifecycleQualification::Decision::kFailure);
}

TEST(MixedLifecycleQualification, MismatchAndUnsafeFilesLeaveOrdinaryPathInactive) {
	PrivateBindingFile mismatch(BindingBody(/*actor=*/43));
	MixedLifecycleQualification qualification(mismatch.path);
	EXPECT_FALSE(qualification.PublishAcceptedGuard(MatchingGuard()));
	EXPECT_EQ(qualification.DecisionForFrame(1),
	          MixedLifecycleQualification::Decision::kInactive);

	PrivateBindingFile unsafe(BindingBody());
	ASSERT_EQ(::chmod(unsafe.path.c_str(), 0644), 0);
	MixedLifecycleQualification unsafe_qualification(unsafe.path);
	EXPECT_FALSE(unsafe_qualification.PublishAcceptedGuard(MatchingGuard()));

	PrivateBindingFile non_guard(BindingBody());
	auto live = MatchingGuard();
	live.set_semantic_action(::highbar::v1::LIVE_SEMANTIC_ACTION_REPAIR);
	MixedLifecycleQualification non_guard_qualification(non_guard.path);
	EXPECT_FALSE(non_guard_qualification.PublishAcceptedGuard(live));

	for (const auto field : {"match", "lifetime", "batch", "client"}) {
		PrivateBindingFile exact(BindingBody());
		auto changed = MatchingGuard();
		if (std::string(field) == "match")
			changed.mutable_binding()->set_match_incarnation(std::string(16, 'x'));
		else if (std::string(field) == "lifetime") changed.mutable_actor()->set_lifetime(8);
		else if (std::string(field) == "batch") changed.mutable_batch()->set_batch_seq(12);
		else changed.mutable_batch()->set_client_command_id(14);
		MixedLifecycleQualification exact_qualification(exact.path);
		EXPECT_FALSE(exact_qualification.PublishAcceptedGuard(changed)) << field;
	}
}

}  // namespace
