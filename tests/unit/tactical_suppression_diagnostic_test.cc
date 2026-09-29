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

}  // namespace
}  // namespace circuit::grpc
