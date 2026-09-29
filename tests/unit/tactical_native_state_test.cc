// SPDX-License-Identifier: GPL-2.0-only
#include "grpc/TacticalNativeState.h"

#include <gtest/gtest.h>

#include <limits>

namespace {
using namespace circuit::grpc;

TEST(TacticalNativeState, RallyQueueApiRequiresExactPreexistingVersionIdentity) {
	EXPECT_TRUE(SupportsRallyQueueApi(
		kRallyQueueEngineHash, kRallyQueueEngineBranch,
		kRallyQueueEngineAdditional));
	EXPECT_FALSE(SupportsRallyQueueApi(
		"7555c82", kRallyQueueEngineBranch, kRallyQueueEngineAdditional));
	EXPECT_FALSE(SupportsRallyQueueApi(
		kRallyQueueEngineHash, "master", kRallyQueueEngineAdditional));
	EXPECT_FALSE(SupportsRallyQueueApi(
		kRallyQueueEngineHash, kRallyQueueEngineBranch, ""));
	EXPECT_FALSE(SupportsRallyQueueApi(nullptr, nullptr, nullptr));
}

TEST(TacticalNativeState, EmptyQueueIsACompleteRevisionNotUnavailable) {
	const auto empty = MakeNativeQueueSnapshot({});
	EXPECT_NE(empty.revision, 0u);
	EXPECT_TRUE(empty.entries.empty());
}

TEST(TacticalNativeState, QueueRevisionCoversOrderTagsOptionsTimeoutAndFloatBits) {
	NativeQueueEntry first{1, 10, 0, 41, 900, {1.0f, -0.0f}};
	NativeQueueEntry second{1, 20, 32, 42, 901, {2.0f}};
	const auto baseline = MakeNativeQueueSnapshot({first, second});
	EXPECT_NE(baseline.revision, 0u);
	EXPECT_TRUE(HasNativeQueueTag(baseline, baseline.revision, 41));
	EXPECT_FALSE(HasNativeQueueTag(baseline, baseline.revision + 1, 41));

	EXPECT_NE(ComputeNativeQueueRevision({second, first}), baseline.revision);
	second.tag = 43;
	EXPECT_NE(ComputeNativeQueueRevision({first, second}), baseline.revision);
	second.tag = 42;
	second.options = 0;
	EXPECT_NE(ComputeNativeQueueRevision({first, second}), baseline.revision);
	second.options = 32;
	second.params = {3.0f};
	EXPECT_NE(ComputeNativeQueueRevision({first, second}), baseline.revision);
}

TEST(TacticalNativeState, FeatureIdZeroIsPresenceSafeAndStableWhileVisible) {
	FeatureLifetimeLedger ledger;
	ASSERT_TRUE(ledger.ReplaceCompleteVisibleSnapshot(10, {{0, 7, 1, 2, 3}}));
	const auto first = ledger.Reference(0);
	ASSERT_TRUE(first.has_value());
	EXPECT_NE(first->lifetime, 0u);
	EXPECT_EQ(first->observed_state_sequence, 10u);
	EXPECT_TRUE(ledger.Matches(*first, {0, 7, 1, 2, 3}));

	ASSERT_TRUE(ledger.ReplaceCompleteVisibleSnapshot(11, {{0, 7, 2, 2, 3}}));
	const auto continuous = ledger.Reference(0);
	ASSERT_TRUE(continuous.has_value());
	EXPECT_EQ(continuous->lifetime, first->lifetime);
	EXPECT_EQ(continuous->observed_state_sequence, 11u);
}

TEST(TacticalNativeState, FeatureAbsenceDefinitionChangeAndDestroyFenceOldReference) {
	FeatureLifetimeLedger ledger;
	ASSERT_TRUE(ledger.ReplaceCompleteVisibleSnapshot(20, {{5, 8, 1, 2, 3}}));
	const auto original = ledger.Reference(5);
	ASSERT_TRUE(original.has_value());

	ASSERT_TRUE(ledger.ReplaceCompleteVisibleSnapshot(21, {}));
	EXPECT_FALSE(ledger.Reference(5).has_value());
	EXPECT_FALSE(ledger.Matches(*original, {5, 8, 1, 2, 3}));

	ASSERT_TRUE(ledger.ReplaceCompleteVisibleSnapshot(22, {{5, 8, 1, 2, 3}}));
	const auto reused = ledger.Reference(5);
	ASSERT_TRUE(reused.has_value());
	EXPECT_NE(reused->lifetime, original->lifetime);

	ASSERT_TRUE(ledger.ReplaceCompleteVisibleSnapshot(23, {{5, 9, 1, 2, 3}}));
	const auto changed = ledger.Reference(5);
	ASSERT_TRUE(changed.has_value());
	EXPECT_NE(changed->lifetime, reused->lifetime);
	EXPECT_FALSE(ledger.Matches(*reused, {5, 9, 1, 2, 3}));

	ledger.MarkDestroyed(5);
	EXPECT_FALSE(ledger.Reference(5).has_value());
	EXPECT_FALSE(ledger.Matches(*changed, {5, 9, 1, 2, 3}));
}

TEST(TacticalNativeState, RefusesIncoherentFeatureSnapshotsWithoutMutation) {
	FeatureLifetimeLedger ledger;
	ASSERT_TRUE(ledger.ReplaceCompleteVisibleSnapshot(30, {{4, 7, 1, 2, 3}}));
	const auto original = ledger.Reference(4);
	ASSERT_TRUE(original.has_value());

	EXPECT_FALSE(ledger.ReplaceCompleteVisibleSnapshot(
		31, {{4, 7, 1, 2, 3}, {4, 7, 1, 2, 3}}));
	EXPECT_FALSE(ledger.ReplaceCompleteVisibleSnapshot(
		31, {{4, 7, std::numeric_limits<float>::quiet_NaN(), 2, 3}}));
	EXPECT_FALSE(ledger.ReplaceCompleteVisibleSnapshot(30, {}));
	const auto retained = ledger.Reference(4);
	ASSERT_TRUE(retained.has_value());
	EXPECT_EQ(retained->lifetime, original->lifetime);
	EXPECT_EQ(retained->observed_state_sequence,
	          original->observed_state_sequence);
}

}  // namespace
