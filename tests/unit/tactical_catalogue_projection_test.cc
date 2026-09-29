// SPDX-License-Identifier: GPL-2.0-only

#include "grpc/TacticalCatalogueProjection.h"

#include <gtest/gtest.h>

namespace circuit::grpc {
namespace {

TEST(TacticalCatalogueProjectionTest, PrefersEngineHumanName) {
	EXPECT_EQ(TacticalCatalogueDisplayName("armcom", "Commander"), "Commander");
}

TEST(TacticalCatalogueProjectionTest, UsesStableInternalNameWhenBarHumanNameIsAbsent) {
	EXPECT_EQ(TacticalCatalogueDisplayName("armcom", nullptr), "armcom");
	EXPECT_EQ(TacticalCatalogueDisplayName("armcom", ""), "armcom");
	EXPECT_EQ(TacticalCatalogueDisplayName("armcom", " \t\n"), "armcom");
}

TEST(TacticalCatalogueProjectionTest, LeavesMissingSourceTextInvalid) {
	EXPECT_TRUE(TacticalCatalogueDisplayName(nullptr, nullptr).empty());
}

}  // namespace
}  // namespace circuit::grpc
