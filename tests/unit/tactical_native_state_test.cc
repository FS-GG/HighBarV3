// SPDX-License-Identifier: GPL-2.0-only
#include "grpc/TacticalNativeState.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>

namespace {
using namespace circuit::grpc;

TEST(TacticalNativeState, QueueEvidenceIsBoundToExactProfileAndRevision) {
	EXPECT_TRUE(TacticalQueueEvidenceMatches(kFullTupleTacticalProfile,
		kFullTupleTacticalRevision, QueueEvidenceScheme::Unspecified));
	EXPECT_TRUE(TacticalQueueEvidenceMatches(kFullTupleTacticalProfile,
		kFullTupleTacticalRevision, QueueEvidenceScheme::FullNativeTupleV1));
	EXPECT_TRUE(TacticalQueueEvidenceMatches(kStockTacticalProfile,
		kStockTacticalRevision, QueueEvidenceScheme::StockLuaSupportedFieldsV1));
	EXPECT_FALSE(TacticalQueueEvidenceMatches(kStockTacticalProfile,
		kStockTacticalRevision, QueueEvidenceScheme::Unspecified));
	EXPECT_FALSE(TacticalQueueEvidenceMatches(kStockTacticalProfile,
		kStockTacticalRevision, QueueEvidenceScheme::FullNativeTupleV1));
	EXPECT_FALSE(TacticalQueueEvidenceMatches(kFullTupleTacticalProfile,
		kFullTupleTacticalRevision, QueueEvidenceScheme::StockLuaSupportedFieldsV1));
	EXPECT_FALSE(TacticalQueueEvidenceMatches(kStockTacticalProfile, 1,
		QueueEvidenceScheme::StockLuaSupportedFieldsV1));
	EXPECT_FALSE(TacticalQueueEvidenceMatches("unknown", 2,
		QueueEvidenceScheme::StockLuaSupportedFieldsV1));
}

TEST(TacticalNativeState, StockFactoryReplaceNeverAliasesAppend) {
	EXPECT_TRUE(StockFactoryProductionPolicyAllows(
		StockFactoryQueuePolicy::Append, false));
	EXPECT_TRUE(StockFactoryProductionPolicyAllows(
		StockFactoryQueuePolicy::Append, true));
	EXPECT_TRUE(StockFactoryProductionPolicyAllows(
		StockFactoryQueuePolicy::RejectIfBusy, true));
	EXPECT_FALSE(StockFactoryProductionPolicyAllows(
		StockFactoryQueuePolicy::RejectIfBusy, false));
	EXPECT_FALSE(StockFactoryProductionPolicyAllows(
		StockFactoryQueuePolicy::Replace, false));
	EXPECT_FALSE(StockFactoryProductionPolicyAllows(
		StockFactoryQueuePolicy::Replace, true));
}

TEST(TacticalNativeState, FinalQueueFenceRunsAfterControlAndBeforeEffect) {
	std::vector<int> events;
	EXPECT_TRUE(DispatchAfterTacticalControlAndFinalFence(false,
		[&] { events.push_back(1); return true; },
		[&] { events.push_back(2); return true; },
		[&] { events.push_back(3); }));
	EXPECT_EQ(events, (std::vector<int>{1, 2, 3}));

	events.clear();
	EXPECT_FALSE(DispatchAfterTacticalControlAndFinalFence(false,
		[&] { events.push_back(1); return true; },
		[&] { events.push_back(2); return false; },
		[&] { events.push_back(3); }));
	EXPECT_EQ(events, (std::vector<int>{1, 2}));
}

TEST(TacticalNativeState, StockProfileRequiresClosedReleaseTuple) {
	EXPECT_TRUE(SupportsStockRecoilProfile("2025", "", "", ""));
	EXPECT_FALSE(SupportsStockRecoilProfile("2025", "custom", "", ""));
	EXPECT_FALSE(SupportsStockRecoilProfile("2026", "", "", ""));
	EXPECT_FALSE(SupportsStockRecoilProfile(nullptr, "", "", ""));
}

StockQueueRevisionContext GoldenStockContext() {
	StockQueueRevisionContext context;
	context.profile=kStockTacticalProfile;context.revision=kStockTacticalRevision;
	context.evidence_scheme=QueueEvidenceScheme::StockLuaSupportedFieldsV1;
	for(int value=0;value<16;++value)context.catalogue_id.push_back(static_cast<char>(value));
	context.catalogue_revision=9007199254741107ULL;context.engine_version="2025.06.19";
	context.game_name="BAR";context.game_version="test-29926-0571aa8";
	for(int value=0;value<32;++value)context.game_content_sha256.push_back(static_cast<char>(value));
	context.actor_id=42;context.actor_lifetime=9007199254741105ULL;context.domain="production";
	return context;
}

TEST(TacticalNativeState, StockRevisionMatchesFrozenGoldenAndEveryBinding) {
	const std::vector<StockQueueEntry> rows{{-710,32,41,{1.0f,-0.0f}}};
	const auto context=GoldenStockContext();const auto preimage=BuildStockQueueRevisionPreimage(context,rows);
	EXPECT_EQ(preimage.size(),285u);
	EXPECT_EQ(Sha256Hex(preimage),"fbd1b79418f394d4e8a8e847c155d43c65e071b76375f9682266453933302c37");
	EXPECT_EQ(ComputeStockQueueRevision(context,rows),18145486220354098388ULL);
	auto changed=context;changed.actor_lifetime++;EXPECT_NE(ComputeStockQueueRevision(changed,rows),ComputeStockQueueRevision(context,rows));
	changed=context;changed.catalogue_id[0]^=1;EXPECT_NE(ComputeStockQueueRevision(changed,rows),ComputeStockQueueRevision(context,rows));
	changed=context;changed.game_content_sha256[0]^=1;EXPECT_NE(ComputeStockQueueRevision(changed,rows),ComputeStockQueueRevision(context,rows));
	changed=context;changed.domain="rally";EXPECT_NE(ComputeStockQueueRevision(changed,rows),ComputeStockQueueRevision(context,rows));
	changed=context;changed.profile=kFullTupleTacticalProfile;EXPECT_EQ(ComputeStockQueueRevision(changed,rows),0u);
	changed=context;changed.evidence_scheme=QueueEvidenceScheme::FullNativeTupleV1;EXPECT_EQ(ComputeStockQueueRevision(changed,rows),0u);
	auto reordered=rows;reordered.push_back({-711,0,42,{}});std::reverse(reordered.begin(),reordered.end());
	EXPECT_NE(ComputeStockQueueRevision(context,reordered),ComputeStockQueueRevision(context,rows));
}

TEST(TacticalNativeState, RallyQueueApiRequiresExactPreexistingVersionIdentity) {
	EXPECT_TRUE(SupportsRallyQueueApi(
		kRallyQueueEngineHash, kRallyQueueEngineAdditional));
	EXPECT_FALSE(SupportsRallyQueueApi(
		"7555c82", kRallyQueueEngineAdditional));
	EXPECT_FALSE(SupportsRallyQueueApi(
		kRallyQueueEngineHash, ""));
	EXPECT_FALSE(SupportsRallyQueueApi(
		kRallyQueueEngineHash, "BARC-01.5-rally-api-v1"));
	EXPECT_FALSE(SupportsRallyQueueApi(nullptr, nullptr));
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

TEST(TacticalNativeState, QueueUnitTargetRequiresExactClassifiedEngineId) {
	NativeQueueEntry entry{1, 25, 0, 41, 900, {17003.0f}};
	EXPECT_EQ(ExactNativeQueueUnitTargetId(entry, true), 17003u);
	const auto target = ResolveNativeQueueUnitTarget(
		entry, true, [](std::uint32_t id) { return id == 17003 ? 7u : 0u; });
	ASSERT_TRUE(target.has_value());
	EXPECT_EQ(target->id, 17003u);
	EXPECT_EQ(target->lifetime, 7u);
	EXPECT_FALSE(ResolveNativeQueueUnitTarget(
		entry, true, [](std::uint32_t) { return 0u; }).has_value());
	EXPECT_FALSE(ExactNativeQueueUnitTargetId(entry, false).has_value());

	entry.params = {};
	EXPECT_FALSE(ExactNativeQueueUnitTargetId(entry, true).has_value());
	entry.params = {17003.0f, 1.0f};
	EXPECT_FALSE(ExactNativeQueueUnitTargetId(entry, true).has_value());
	entry.params = {17003.5f};
	EXPECT_FALSE(ExactNativeQueueUnitTargetId(entry, true).has_value());
	entry.params = {-1.0f};
	EXPECT_FALSE(ExactNativeQueueUnitTargetId(entry, true).has_value());
	entry.params = {32000.0f};
	EXPECT_FALSE(ExactNativeQueueUnitTargetId(entry, true).has_value());
	entry.params = {std::numeric_limits<float>::quiet_NaN()};
	EXPECT_FALSE(ExactNativeQueueUnitTargetId(entry, true).has_value());
}

TEST(TacticalNativeState, FinalQueueFenceRequiresExactRevisionAndNativeTag) {
	const std::vector<NativeQueueEntry> current{
		{1, 10, 0, 41, 900, {1.0f}},
		{1, 20, 32, 42, 901, {2.0f}},
	};
	const auto revision = ComputeNativeQueueRevision(current);
	EXPECT_TRUE(NativeQueueMatchesExpected(current, revision));
	EXPECT_TRUE(NativeQueueMatchesExpected(current, revision, 42));
	EXPECT_FALSE(NativeQueueMatchesExpected(current, revision, 99));
	EXPECT_FALSE(NativeQueueMatchesExpected(current, revision + 1, 42));
	EXPECT_FALSE(NativeQueueMatchesExpected(current, 0));
	auto changed = current;
	changed[1].options = 0;
	EXPECT_FALSE(NativeQueueMatchesExpected(changed, revision, 42));
}

TEST(TacticalNativeState, AcceptedEffectAcquiresControlBeforeDispatch) {
	bool controlled = false;
	bool effect_observed_control = false;
	int acquisitions = 0;
	EXPECT_TRUE(DispatchAfterTacticalControlFence(false, [&] {
		++acquisitions;
		controlled = true;
		return true;
	}, [&] {
		effect_observed_control = controlled;
	}));
	EXPECT_EQ(acquisitions, 1);
	EXPECT_TRUE(effect_observed_control);
}

TEST(TacticalNativeState, ExistingControlIsStableAndFailedFenceHasNoEffect) {
	int acquisitions = 0;
	int effects = 0;
	EXPECT_TRUE(DispatchAfterTacticalControlFence(true, [&] {
		++acquisitions;
		return false;
	}, [&] { ++effects; }));
	EXPECT_EQ(acquisitions, 0);
	EXPECT_EQ(effects, 1);

	EXPECT_FALSE(DispatchAfterTacticalControlFence(false, [&] {
		++acquisitions;
		return false;
	}, [&] { ++effects; }));
	EXPECT_EQ(acquisitions, 1);
	EXPECT_EQ(effects, 1);
}

TEST(TacticalNativeState, ActorWithoutAutonomousOwnerDispatchesWithoutAcquisition) {
	int acquisitions = 0;
	int effects = 0;
	EXPECT_TRUE(DispatchAfterTacticalControlFence(true, [&] {
		++acquisitions;
		return false;
	}, [&] { ++effects; }));
	EXPECT_EQ(acquisitions, 0);
	EXPECT_EQ(effects, 1);
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
