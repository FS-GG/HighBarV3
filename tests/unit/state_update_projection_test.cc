// SPDX-License-Identifier: GPL-2.0-only

#include "grpc/StateUpdateProjection.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <stdexcept>

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
	legacy.mutable_delta()->add_events()->mutable_unit_damaged()->set_unit_id(42);
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

// Controlled engine observations, not a replay of retained native payloads.
// The retained packet has sequence/arm observations but no raw StateDelta.
TEST(StateUpdateProjection, OwnedDamageBurstSerializesAuthoritativeReplacement) {
	StateUpdateOrder order;
	::highbar::v1::StateUpdate legacy;
	legacy.set_seq(35);
	legacy.set_frame(900);
	legacy.set_send_monotonic_ns(900000);
	for (int i = 0; i < 128; ++i) {
		auto* event = legacy.mutable_delta()->add_events()->mutable_unit_damaged();
		event->set_unit_id(42);
		event->set_damage(25.0f);
		event->set_is_paralyzer(i % 2 == 0);
		order.RequestFullStateReplacement();
	}
	legacy.mutable_delta()->add_events()->mutable_command_dispatch()->set_frame(900);
	legacy.mutable_delta()->add_events()->mutable_economy_tick()->set_metal(250.0f);
	const auto original = legacy.SerializeAsString();
	const auto plan = order.Plan(true, true);
	ASSERT_FALSE(plan.snapshot_before_delta);
	ASSERT_TRUE(plan.flush_delta);
	ASSERT_TRUE(plan.snapshot_after_delta);
	ASSERT_FALSE(order.ReplacementPending());
	::highbar::v1::StateUpdate projected;
	ASSERT_TRUE(BuildCoordinatorDeltaProjection(legacy, &projected));
	ASSERT_EQ(projected.delta().events_size(), 2);
	EXPECT_EQ(projected.seq(), 35u);
	EXPECT_EQ(projected.frame(), 900u);
	EXPECT_EQ(projected.send_monotonic_ns(), 900000u);
	EXPECT_EQ(projected.delta().events(0).kind_case(), ::highbar::v1::DeltaEvent::kCommandDispatch);
	EXPECT_EQ(projected.delta().events(1).kind_case(), ::highbar::v1::DeltaEvent::kEconomyTick);
	EXPECT_EQ(legacy.SerializeAsString(), original);
	// The authoritative controlled observation is 83, regardless of 128 damage
	// notifications, repair/regeneration, or paralyzer damage. No subtraction.
	::highbar::v1::StateUpdate replacement;
	replacement.set_seq(36);
	replacement.set_frame(900);
	replacement.set_send_monotonic_ns(900001);
	auto* snapshot = replacement.mutable_snapshot();
	snapshot->set_frame_number(900);
	snapshot->set_effective_cadence_frames(1);
	auto* unit = snapshot->add_own_units();
	unit->set_unit_id(42);
	unit->set_team_id(0);
	unit->set_def_id(7);
	unit->set_health(83.0f);
	unit->set_max_health(100.0f);
	unit->mutable_position()->set_x(100.0f);
	unit->mutable_position()->set_z(200.0f);
	snapshot->mutable_economy()->set_metal(250.0f);
	::highbar::v1::StateUpdate decoded;
	ASSERT_TRUE(decoded.ParseFromString(replacement.SerializeAsString()));
	EXPECT_EQ(decoded.snapshot().own_units(0).health(), 83.0f);
	EXPECT_EQ(decoded.snapshot().own_units_size(), 1);
	EXPECT_FALSE(decoded.snapshot().has_static_map());
	const auto next = order.Plan(false, false);
	EXPECT_FALSE(next.snapshot_before_delta);
	EXPECT_FALSE(next.snapshot_after_delta);
	EXPECT_FALSE(next.flush_delta);
	if (const char* output = std::getenv("HIGHBAR_CONTROLLED_FIXTURE_DIR")) {
		std::filesystem::create_directories(output);
		const auto write = [&](const char* name, const auto& update) {
			std::ofstream file(std::filesystem::path(output) / name, std::ios::binary);
			ASSERT_TRUE(file.good());
			ASSERT_TRUE(update.SerializeToOstream(&file));
		};
		auto baseline = replacement;
		baseline.set_seq(34);
		baseline.set_frame(899);
		baseline.set_send_monotonic_ns(899001);
		baseline.mutable_snapshot()->set_frame_number(899);
		baseline.mutable_snapshot()->mutable_own_units(0)->set_health(100.0f);
		write("owned-damage-baseline.pb", baseline);
		write("owned-damage-legacy.pb", legacy);
		write("owned-damage-projected.pb", projected);
		write("owned-damage-replacement.pb", replacement);
	}
}

TEST(StateUpdateProjection, FailedBuildOrEnqueueCannotAdvanceBasis) {
	using ::circuit::grpc::PublishSnapshotBeforeBasis;
	int enqueue_calls = 0;
	std::uint64_t basis = 34;
	const auto enqueue = [&](const auto&) { ++enqueue_calls; return false; };
	const auto record = [&](const auto& update) { basis = update.seq(); };
	EXPECT_THROW(PublishSnapshotBeforeBasis([]() -> ::highbar::v1::StateUpdate {
		throw std::runtime_error("controlled build/serialization failure");
	}, enqueue, record), std::runtime_error);
	EXPECT_EQ(enqueue_calls, 0);
	EXPECT_EQ(basis, 34u);
	EXPECT_FALSE(PublishSnapshotBeforeBasis([]() {
		::highbar::v1::StateUpdate update;
		update.set_seq(36);
		update.mutable_snapshot();
		return update;
	}, enqueue, record));
	EXPECT_EQ(enqueue_calls, 1);
	EXPECT_EQ(basis, 34u);
}

TEST(StateUpdateProjection, BasisUsesExactEnqueuedSnapshotIdentity) {
	using ::circuit::grpc::PublishSnapshotBeforeBasis;
	std::string enqueued_bytes;
	::highbar::v1::StateUpdate recorded;
	ASSERT_TRUE(PublishSnapshotBeforeBasis([]() {
		::highbar::v1::StateUpdate update;
		update.set_seq(36);
		update.set_frame(900);
		update.set_send_monotonic_ns(900001);
		update.mutable_snapshot()->set_frame_number(900);
		return update;
	}, [&](const auto& update) {
		enqueued_bytes = update.SerializeAsString();
		return true;
	}, [&](const auto& update) { recorded = update; }));
	EXPECT_EQ(recorded.SerializeAsString(), enqueued_bytes);
	EXPECT_EQ(recorded.seq(), 36u);
	EXPECT_EQ(recorded.frame(), 900u);
	EXPECT_EQ(recorded.send_monotonic_ns(), 900001u);
}

TEST(StateUpdateProjection, OwnedDamageOnlySkipsSparseSequence) {
	::highbar::v1::StateUpdate legacy;
	legacy.set_seq(35);
	legacy.mutable_delta()->add_events()->mutable_unit_damaged()->set_unit_id(42);
	::highbar::v1::StateUpdate projected;
	EXPECT_FALSE(BuildCoordinatorDeltaProjection(legacy, &projected));
	EXPECT_EQ(projected.delta().events_size(), 0);
}

TEST(StateUpdateProjection, UnscheduledReplacementRemainsPending) {
	StateUpdateOrder order;
	order.RequestFullStateReplacement();
	const auto deferred = order.Plan(false, true);
	EXPECT_FALSE(deferred.snapshot_before_delta);
	EXPECT_FALSE(deferred.snapshot_after_delta);
	EXPECT_TRUE(deferred.flush_delta);
	EXPECT_TRUE(order.ReplacementPending());
	EXPECT_TRUE(order.Plan(true, false).snapshot_after_delta);
}

TEST(StateUpdateProjection, LifecycleArmsRemainVisibleForConsumerRefusal) {
	::highbar::v1::StateUpdate source;
	source.mutable_delta()->add_events()->mutable_unit_damaged()->set_unit_id(42);
	source.mutable_delta()->add_events()->mutable_unit_created()->set_unit_id(43);
	source.mutable_delta()->add_events()->mutable_unit_destroyed()->set_unit_id(44);
	::highbar::v1::StateUpdate projected;
	ASSERT_TRUE(BuildCoordinatorDeltaProjection(source, &projected));
	ASSERT_EQ(projected.delta().events_size(), 2);
	EXPECT_EQ(projected.delta().events(0).kind_case(), ::highbar::v1::DeltaEvent::kUnitCreated);
	EXPECT_EQ(projected.delta().events(1).kind_case(), ::highbar::v1::DeltaEvent::kUnitDestroyed);
}

TEST(StateUpdateProjection, OwnedCallbackRequestsReplacementWithoutInferringHealth) {
	const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
	const auto source = ReadSource(root / "src/circuit/module/GrpcGatewayModule.cpp");
	const auto begin = source.find("void CGrpcGatewayModule::OnUnitDamagedFull");
	const auto end = source.find("int CGrpcGatewayModule::UnitDestroyed", begin);
	ASSERT_NE(begin, std::string::npos);
	ASSERT_NE(end, std::string::npos);
	const auto callback = source.substr(begin, end - begin);
	EXPECT_NE(callback.find("mutable_unit_damaged()"), std::string::npos);
	EXPECT_NE(callback.find("RequestFullStateReplacement();"), std::string::npos);
	EXPECT_EQ(callback.find("set_health"), std::string::npos);
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
