// SPDX-License-Identifier: GPL-2.0-only

#include "grpc/TacticalDispatchDiagnostic.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>
#include <unordered_set>

namespace circuit::grpc {
namespace {

TEST(TacticalDispatchDiagnosticTest, BuildPredicatesHaveDistinctBoundedLabels) {
	using R = TacticalDispatchRefusalReason;
	constexpr std::array reasons{
		R::kUnsupportedOrInvalidArm,
		R::kInvalidContext,
		R::kBuildDefinitionMissing,
		R::kBuildCapabilityChanged,
		R::kBuildMapUnavailable,
		R::kBuildFacingInvalid,
		R::kBuildPositionNonFinite,
		R::kBuildSiteUnavailable,
	};
	std::unordered_set<std::string_view> labels;
	for (const auto reason : reasons) {
		const std::string_view label = TacticalDispatchRefusalReasonName(reason);
		EXPECT_FALSE(label.empty());
		EXPECT_LE(label.size(), 32u);
		EXPECT_EQ(label.find_first_not_of("abcdefghijklmnopqrstuvwxyz_"),
			std::string_view::npos);
		EXPECT_TRUE(labels.insert(label).second);
	}
}

TEST(TacticalDispatchDiagnosticTest, BuildCapabilityAndSiteRemainSeparate) {
	using R = TacticalDispatchRefusalReason;
	EXPECT_EQ(std::string_view(TacticalDispatchRefusalReasonName(
		R::kBuildCapabilityChanged)), "build_capability_changed");
	EXPECT_EQ(std::string_view(TacticalDispatchRefusalReasonName(
		R::kBuildSiteUnavailable)), "build_site_unavailable");
}

}  // namespace
}  // namespace circuit::grpc
