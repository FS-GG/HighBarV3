// SPDX-License-Identifier: GPL-2.0-only

#include "module/TacticalSuppressionDiagnostic.h"

#include <gtest/gtest.h>

namespace circuit::grpc {
namespace {

TEST(TacticalSuppressionDiagnosticTest, FeatureLimitClassifiesExactBoundary) {
	EXPECT_FALSE(TacticalCountExceedsLimit(256, 256));
	EXPECT_TRUE(TacticalCountExceedsLimit(257, 256));
}

TEST(TacticalSuppressionDiagnosticTest, ThrottlesRepeatedReasonUntilWindow) {
	TacticalSuppressionTraceThrottle throttle(2);
	const TacticalSuppressionObservation overflow{
		TacticalSuppressionReason::FeatureOverflow,
		TacticalSuppressionSource::None, 257, 256};

	EXPECT_TRUE(throttle.ShouldEmit(overflow));
	EXPECT_FALSE(throttle.ShouldEmit(overflow));
	EXPECT_FALSE(throttle.ShouldEmit(overflow));
	EXPECT_TRUE(throttle.ShouldEmit(overflow));
}

TEST(TacticalSuppressionDiagnosticTest, ChangedReasonOrCountEmitsImmediately) {
	TacticalSuppressionTraceThrottle throttle(128);
	EXPECT_TRUE(throttle.ShouldEmit({
		TacticalSuppressionReason::FeatureOverflow,
		TacticalSuppressionSource::None, 257, 256}));
	EXPECT_FALSE(throttle.ShouldEmit({
		TacticalSuppressionReason::FeatureOverflow,
		TacticalSuppressionSource::None, 257, 256}));
	EXPECT_TRUE(throttle.ShouldEmit({
		TacticalSuppressionReason::FeatureOverflow,
		TacticalSuppressionSource::None, 258, 256}));
	EXPECT_TRUE(throttle.ShouldEmit({
		TacticalSuppressionReason::FeatureLedgerRejected,
		TacticalSuppressionSource::None, 258, 256}));
}

TEST(TacticalSuppressionDiagnosticTest, ResetMakesNextSuppressionFirstAgain) {
	TacticalSuppressionTraceThrottle throttle(128);
	const TacticalSuppressionObservation unavailable{
		TacticalSuppressionReason::SourceUnavailable,
		TacticalSuppressionSource::Callback};
	EXPECT_TRUE(throttle.ShouldEmit(unavailable));
	EXPECT_FALSE(throttle.ShouldEmit(unavailable));
	throttle.Reset();
	EXPECT_TRUE(throttle.ShouldEmit(unavailable));
}

TEST(TacticalSuppressionDiagnosticTest, MessageContainsOnlyBoundedReasonAndCounts) {
	const auto message = TacticalSuppressionTraceMessage({
		TacticalSuppressionReason::DescriptorOverflow,
		TacticalSuppressionSource::None, 33, 32});
	EXPECT_EQ(message,
		"tactical snapshot suppressed reason=descriptor_overflow source=none observed=33 limit=32");
	EXPECT_LT(message.size(), 160u);
}

TEST(TacticalSuppressionDiagnosticTest, TraceDisabledDoesNotReadCallbackMode) {
	int reads = 0;
	const auto mode = ReadCallbackFeatureVisibilityMode(false, [&] {
		++reads;
		return CallbackFeatureVisibilityMode::CheatsAllActive;
	});
	EXPECT_FALSE(mode.has_value());
	EXPECT_EQ(reads, 0);
}

TEST(TacticalSuppressionDiagnosticTest, FeatureObservationNamesActualEnginePolicy) {
	const auto normal = TacticalFeatureObservationTraceMessage({
		CallbackFeatureVisibilityMode::NormalAllyTeamLos,
		344, 344, 344, true, 512});
	EXPECT_EQ(normal,
		"tactical feature observation mode=normal "
		"visibility_policy=engine_feature_is_in_los_for_allyteam "
		"raw=344 valid=344 emitted=344 complete=1 limit=512");
	const auto cheats = TacticalFeatureObservationTraceMessage({
		CallbackFeatureVisibilityMode::CheatsAllActive,
		513, std::nullopt, 0, false, 512});
	EXPECT_EQ(cheats,
		"tactical feature observation mode=cheats "
		"visibility_policy=engine_all_active_features "
		"raw=513 valid=unknown emitted=0 complete=0 limit=512");
	EXPECT_LT(normal.size(), 200u);
	EXPECT_LT(cheats.size(), 200u);
}

TEST(TacticalSuppressionDiagnosticTest, FeatureObservationEmitsFirstAndChangesOnly) {
	TacticalFeatureObservationTraceState state;
	TacticalFeatureObservation observation{
		CallbackFeatureVisibilityMode::NormalAllyTeamLos,
		344, 344, 344, true, 512};
	EXPECT_TRUE(state.MaybeMessage(observation).has_value());
	EXPECT_FALSE(state.MaybeMessage(observation).has_value());
	observation.raw = 345;
	observation.valid = 345;
	observation.emitted = 345;
	EXPECT_TRUE(state.MaybeMessage(observation).has_value());
	EXPECT_FALSE(state.MaybeMessage(observation).has_value());
	observation.mode = CallbackFeatureVisibilityMode::CheatsAllActive;
	EXPECT_TRUE(state.MaybeMessage(observation).has_value());
}

}  // namespace
}  // namespace circuit::grpc
