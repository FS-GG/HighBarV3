// SPDX-License-Identifier: GPL-2.0-only

#include "grpc/TacticalNativeState.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace circuit::grpc {
namespace {

std::vector<VisibleFeatureSample> Features(std::size_t count) {
	std::vector<VisibleFeatureSample> features;
	features.reserve(count);
	for (std::size_t i = 0; i < count; ++i) {
		features.push_back({static_cast<std::uint32_t>(1000 + i),
			static_cast<std::uint32_t>(2000 + i),
			static_cast<float>(i), 0.0f, static_cast<float>(i + 1)});
	}
	return features;
}

TEST(TacticalFeatureCapacity, AdmitsCompletePopulationsThrough512AndRefuses513) {
	for (const auto count : {std::size_t{256}, std::size_t{342},
			std::size_t{512}}) {
		FeatureLifetimeLedger ledger;
		auto features = Features(count);
		ASSERT_TRUE(ledger.ReplaceBoundedCompleteVisibleSnapshot(
			1, count, features));
		EXPECT_TRUE(ledger.Reference(features.back().id).has_value());
	}

	FeatureLifetimeLedger ledger;
	auto admitted = Features(512);
	ASSERT_TRUE(ledger.ReplaceBoundedCompleteVisibleSnapshot(1, 512, admitted));
	auto overflow = Features(513);
	EXPECT_FALSE(ledger.ReplaceBoundedCompleteVisibleSnapshot(2, 513, overflow));
	EXPECT_FALSE(ledger.Reference(admitted.front().id).has_value());
}

TEST(TacticalFeatureCapacity, StableReferencesCoverEntriesBeyondOldCapacity) {
	FeatureLifetimeLedger ledger;
	auto features = Features(512);
	ASSERT_TRUE(ledger.ReplaceBoundedCompleteVisibleSnapshot(1, 512, features));
	const auto at_256 = ledger.Reference(features[256].id);
	const auto at_511 = ledger.Reference(features[511].id);
	ASSERT_TRUE(at_256.has_value());
	ASSERT_TRUE(at_511.has_value());
	ASSERT_TRUE(ledger.ReplaceBoundedCompleteVisibleSnapshot(2, 512, features));
	EXPECT_EQ(ledger.Reference(features[256].id)->lifetime, at_256->lifetime);
	EXPECT_EQ(ledger.Reference(features[511].id)->lifetime, at_511->lifetime);
}

TEST(TacticalFeatureCapacity, NullInvalidAndDuplicateSamplesCannotClaimComplete) {
	FeatureLifetimeLedger ledger;
	auto valid = Features(2);
	ASSERT_TRUE(ledger.ReplaceBoundedCompleteVisibleSnapshot(1, 2, valid));

	// A null or otherwise invalid native sample is omitted during extraction,
	// leaving the raw and valid counts unequal.
	EXPECT_FALSE(ledger.ReplaceBoundedCompleteVisibleSnapshot(2, 2, {valid[0]}));
	EXPECT_FALSE(ledger.Reference(valid[0].id).has_value());

	auto duplicate = Features(2);
	duplicate[1].id = duplicate[0].id;
	EXPECT_FALSE(ledger.ReplaceBoundedCompleteVisibleSnapshot(3, 2, duplicate));
	EXPECT_FALSE(ledger.Reference(duplicate[0].id).has_value());
}

TEST(TacticalFeatureCapacity, OverflowInvalidatesPreviouslyAdmittedReclaim) {
	FeatureLifetimeLedger ledger;
	auto valid = Features(342);
	ASSERT_TRUE(ledger.ReplaceBoundedCompleteVisibleSnapshot(1, valid.size(), valid));
	const auto stale = ledger.Reference(valid[300].id);
	ASSERT_TRUE(stale.has_value());

	auto overflow = Features(513);
	EXPECT_FALSE(ledger.ReplaceBoundedCompleteVisibleSnapshot(2, overflow.size(), overflow));
	int engine_calls = 0;
	EXPECT_FALSE(DispatchCurrentFeatureReclaim(
		&ledger, *stale, valid[300], [&] { ++engine_calls; }));
	EXPECT_EQ(engine_calls, 0);
}

}  // namespace
}  // namespace circuit::grpc
