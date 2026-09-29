// SPDX-License-Identifier: GPL-2.0-only
//
// HighBarV3 — command drain integration coverage (feature 011).
//
// This keeps the authoritative-target regression anchored at the same
// queue/drain seam the gateway uses on the engine thread, without
// depending on the dlopen mock-engine harness.

#include "grpc/CommandDispatch.h"
#include "grpc/CommandQueue.h"

#include <gtest/gtest.h>

namespace {

using circuit::grpc::CommandQueue;
using circuit::grpc::EffectiveDispatchTargetUnitId;
using circuit::grpc::QueuedCommand;

QueuedCommand MakeMoveCommand(
		std::int32_t authoritative_target_unit_id,
		std::int32_t command_unit_id) {
	QueuedCommand queued;
	queued.authoritative_target_unit_id = authoritative_target_unit_id;
	auto* move = queued.command.mutable_move_unit();
	move->set_unit_id(command_unit_id);
	move->mutable_to_position()->set_x(0.0f);
	move->mutable_to_position()->set_y(0.0f);
	move->mutable_to_position()->set_z(0.0f);
	return queued;
}

TEST(AiMoveFlow, DrainKeepsAuthoritativeBatchTargetForUnitCommands) {
	CommandQueue queue(/*counters=*/nullptr, /*capacity=*/4);
	ASSERT_TRUE(queue.TryPush(MakeMoveCommand(/*authoritative_target=*/42,
	                                        /*embedded_command_unit=*/99)));

	std::vector<QueuedCommand> drained;
	ASSERT_EQ(queue.Drain(&drained), 1u);
	ASSERT_EQ(drained.size(), 1u);
	ASSERT_EQ(drained[0].authoritative_target_unit_id, 42);
	ASSERT_EQ(drained[0].command.move_unit().unit_id(), 99);

	const auto dispatch_target = EffectiveDispatchTargetUnitId(
		drained[0].authoritative_target_unit_id,
		drained[0].command);
	ASSERT_TRUE(dispatch_target.has_value());
	EXPECT_EQ(*dispatch_target, 42);
}

TEST(AiMoveFlow, DrainNormalizesGameWideCommandsToSyntheticTarget) {
	CommandQueue queue(/*counters=*/nullptr, /*capacity=*/4);
	QueuedCommand queued;
	queued.authoritative_target_unit_id = 77;
	queued.command.mutable_send_text_message()->set_text("hold position");
	ASSERT_TRUE(queue.TryPush(std::move(queued)));

	std::vector<QueuedCommand> drained;
	ASSERT_EQ(queue.Drain(&drained), 1u);
	ASSERT_EQ(drained.size(), 1u);

	const auto dispatch_target = EffectiveDispatchTargetUnitId(
		drained[0].authoritative_target_unit_id,
		drained[0].command);
	ASSERT_TRUE(dispatch_target.has_value());
	EXPECT_EQ(*dispatch_target, -1);
}

TEST(AiMoveFlow, DrainPreservesLegalUnitZeroTarget) {
	CommandQueue queue(/*counters=*/nullptr, /*capacity=*/4);
	ASSERT_TRUE(queue.TryPush(MakeMoveCommand(/*authoritative_target=*/0,
	                                        /*embedded_command_unit=*/42)));

	std::vector<QueuedCommand> drained;
	ASSERT_EQ(queue.Drain(&drained), 1u);
	const auto dispatch_target = EffectiveDispatchTargetUnitId(
		drained[0].authoritative_target_unit_id,
		drained[0].command);
	ASSERT_TRUE(dispatch_target.has_value());
	EXPECT_EQ(*dispatch_target, 0);
}

}  // namespace
