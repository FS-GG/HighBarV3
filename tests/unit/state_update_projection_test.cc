// SPDX-License-Identifier: GPL-2.0-only

#include "grpc/StateUpdateProjection.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using ::circuit::grpc::BuildCoordinatorDeltaProjection;
using ::circuit::grpc::StateUpdateOrder;

std::string ReadSource(const std::filesystem::path& path) {
	std::ifstream input(path);
	EXPECT_TRUE(input.good()) << path;
	std::ostringstream buffer;
	buffer << input.rdbuf();
	return buffer.str();
}

TEST(StateUpdateProjection, PeriodicSnapshotRetainsSnapshotThenDeltaOrder) {
	StateUpdateOrder order;
	const auto plan = order.Plan(/*snapshot_scheduled=*/true, /*delta_pending=*/true);
	EXPECT_TRUE(plan.snapshot_before_delta);
	EXPECT_TRUE(plan.flush_delta);
	EXPECT_FALSE(plan.snapshot_after_delta);
}

TEST(StateUpdateProjection, ReplacementFollowsProjectedDeltaAndCoalesces) {
	StateUpdateOrder order;
	order.RequestFullStateReplacement();
	order.RequestFullStateReplacement();
	const auto plan = order.Plan(/*snapshot_scheduled=*/true, /*delta_pending=*/true);
	EXPECT_FALSE(plan.snapshot_before_delta);
	EXPECT_TRUE(plan.flush_delta);
	EXPECT_TRUE(plan.snapshot_after_delta);
	EXPECT_FALSE(order.ReplacementPending());
}

TEST(StateUpdateProjection, LegacyBytesStayExactAndCoordinatorKeepsFeedback) {
	::highbar::v1::StateUpdate legacy;
	legacy.set_seq(113);
	legacy.set_frame(900);
	legacy.mutable_delta()->add_events()->mutable_enemy_damaged()->set_enemy_id(77);
	legacy.mutable_delta()->add_events()->mutable_command_dispatch()->set_frame(900);
	legacy.mutable_delta()->add_events()->mutable_economy_tick()->set_metal(250.0f);
	legacy.mutable_delta()->add_events()->mutable_unit_idle()->set_unit_id(42);
	auto* unknown = legacy.mutable_delta()->add_events();
	const std::string unknown_arm("\xe2\x01\x00", 3);  // reserved field 28
	ASSERT_TRUE(unknown->ParseFromString(unknown_arm));
	std::string before;
	ASSERT_TRUE(legacy.SerializeToString(&before));

	::highbar::v1::StateUpdate projected;
	ASSERT_TRUE(BuildCoordinatorDeltaProjection(legacy, &projected));
	ASSERT_EQ(projected.seq(), 113u);
	ASSERT_EQ(projected.frame(), 900u);
	ASSERT_EQ(projected.delta().events_size(), 4);
	EXPECT_EQ(projected.delta().events(0).kind_case(),
	          ::highbar::v1::DeltaEvent::kCommandDispatch);
	EXPECT_EQ(projected.delta().events(1).kind_case(),
	          ::highbar::v1::DeltaEvent::kEconomyTick);
	EXPECT_EQ(projected.delta().events(2).kind_case(),
	          ::highbar::v1::DeltaEvent::kUnitIdle);
	EXPECT_EQ(projected.delta().events(3).kind_case(),
	          ::highbar::v1::DeltaEvent::KIND_NOT_SET);
	EXPECT_EQ(projected.delta().events(3).SerializeAsString(), unknown_arm);

	std::string after;
	ASSERT_TRUE(legacy.SerializeToString(&after));
	EXPECT_EQ(after, before) << "coordinator projection mutated legacy ring/bus bytes";
}

TEST(StateUpdateProjection, SparseOnlyDamageDoesNotReachCoordinatorAsDelta) {
	::highbar::v1::StateUpdate legacy;
	legacy.set_seq(113);
	legacy.mutable_delta()->add_events()->mutable_enemy_damaged()->set_enemy_id(77);
	::highbar::v1::StateUpdate projected;
	EXPECT_FALSE(BuildCoordinatorDeltaProjection(legacy, &projected));
	EXPECT_EQ(projected.delta().events_size(), 0);
}

TEST(StateUpdateProjection, SparseOnlyDestroyDoesNotReachCoordinatorAsDelta) {
	::highbar::v1::StateUpdate legacy;
	legacy.set_seq(200);
	legacy.mutable_delta()->add_events()->mutable_enemy_destroyed()->set_enemy_id(77);
	::highbar::v1::StateUpdate projected;
	EXPECT_FALSE(BuildCoordinatorDeltaProjection(legacy, &projected));
}

TEST(StateUpdateProjection, DamageBoundaryRefreshesSpringCacheBeforeReplacement) {
	const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
	const auto source = ReadSource(root / "src/circuit/module/GrpcGatewayModule.cpp");
	const auto callback = source.find("void CGrpcGatewayModule::OnEnemyDamaged");
	const auto refresh = source.find("enemy->GetData()->UpdateInLosData();", callback);
	const auto legacy = source.find("mutable_enemy_damaged()", callback);
	const auto replacement = source.find("RequestFullStateReplacement();", callback);
	ASSERT_NE(callback, std::string::npos);
	ASSERT_NE(refresh, std::string::npos);
	ASSERT_LT(refresh, legacy);
	ASSERT_LT(legacy, replacement);
}

TEST(StateUpdateProjection, DestroyBoundarySnapshotReadsPostRemovalEnemySet) {
	const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
	const auto circuit = ReadSource(root / "src/circuit/CircuitAI.cpp");
	const auto manager = ReadSource(root / "src/circuit/unit/enemy/EnemyManager.cpp");
	const auto snapshot = ReadSource(root / "src/circuit/grpc/SnapshotBuilder.cpp");

	const auto destroyed = circuit.find("case EVENT_ENEMY_DESTROYED:");
	const auto dying = circuit.find("allyTeam->DyingEnemy(enemy->GetData(), lastFrame);", destroyed);
	const auto next_case = circuit.find("case EVENT_", destroyed + 5);
	ASSERT_NE(destroyed, std::string::npos);
	ASSERT_NE(dying, std::string::npos);
	ASSERT_LT(dying, next_case);

	const auto unregister = manager.find("void CEnemyManager::UnregisterEnemyUnit");
	const auto erase = manager.find("enemyUnits.erase(data->GetId());", unregister);
	const auto unregister_end = manager.find("\n}", unregister);
	ASSERT_NE(unregister, std::string::npos);
	ASSERT_NE(erase, std::string::npos);
	ASSERT_LT(erase, unregister_end);

	const auto fill = snapshot.find("void SnapshotBuilder::FillEnemies");
	const auto enumerate = snapshot.find("em->GetEnemyUnits()", fill);
	const auto fill_end = snapshot.find("void SnapshotBuilder::FillFeatures", fill);
	ASSERT_NE(fill, std::string::npos);
	ASSERT_NE(enumerate, std::string::npos);
	ASSERT_LT(enumerate, fill_end);
}

}  // namespace
