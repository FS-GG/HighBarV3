// SPDX-License-Identifier: GPL-2.0-only
#include "grpc/TacticalNativeState.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace circuit::grpc {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void AddBytes(std::uint64_t* hash, const void* data, std::size_t size) {
	const auto* bytes = static_cast<const unsigned char*>(data);
	for (std::size_t i = 0; i < size; ++i) {
		*hash ^= bytes[i];
		*hash *= kFnvPrime;
	}
}

template <typename T>
void AddScalar(std::uint64_t* hash, const T& value) {
	AddBytes(hash, &value, sizeof(value));
}

std::uint32_t FloatBits(float value) {
	std::uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value));
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

void AdvanceLifetime(std::uint64_t* lifetime) {
	++*lifetime;
	if (*lifetime == 0) ++*lifetime;
}

}  // namespace

FeatureLifetimeLedger::Entry* FeatureLifetimeLedger::Find(std::uint32_t id) {
	for (auto& item : entries_) {
		if (item.first == id) return &item.second;
	}
	entries_.push_back({id, {}});
	return &entries_.back().second;
}

const FeatureLifetimeLedger::Entry* FeatureLifetimeLedger::Find(
		std::uint32_t id) const {
	for (const auto& item : entries_) {
		if (item.first == id) return &item.second;
	}
	return nullptr;
}

std::uint64_t ComputeNativeQueueRevision(
		const std::vector<NativeQueueEntry>& entries) {
	std::uint64_t hash = kFnvOffset;
	const auto count = static_cast<std::uint64_t>(entries.size());
	AddScalar(&hash, count);
	for (const auto& entry : entries) {
		AddScalar(&hash, entry.type);
		AddScalar(&hash, entry.command_id);
		AddScalar(&hash, entry.options);
		AddScalar(&hash, entry.tag);
		AddScalar(&hash, entry.timeout);
		const auto param_count = static_cast<std::uint64_t>(entry.params.size());
		AddScalar(&hash, param_count);
		for (float param : entry.params) {
			const auto bits = FloatBits(param);
			AddScalar(&hash, bits);
		}
	}
	// Zero is reserved for an absent/unavailable observation on the wire.
	return hash == 0 ? 1 : hash;
}

NativeQueueSnapshot MakeNativeQueueSnapshot(
		std::vector<NativeQueueEntry> entries) {
	NativeQueueSnapshot snapshot;
	snapshot.revision = ComputeNativeQueueRevision(entries);
	snapshot.entries = std::move(entries);
	return snapshot;
}

bool HasNativeQueueTag(const NativeQueueSnapshot& snapshot,
		std::uint64_t expected_revision, std::int32_t tag) {
	if (expected_revision == 0 || snapshot.revision != expected_revision) {
		return false;
	}
	return std::any_of(snapshot.entries.begin(), snapshot.entries.end(),
		[tag](const NativeQueueEntry& entry) { return entry.tag == tag; });
}

bool FeatureLifetimeLedger::ReplaceCompleteVisibleSnapshot(
		std::uint64_t state_sequence,
		const std::vector<VisibleFeatureSample>& features) {
	if (state_sequence == 0 || state_sequence <= last_complete_state_sequence_) {
		return false;
	}
	std::unordered_set<std::uint32_t> seen;
	seen.reserve(features.size());
	for (const auto& feature : features) {
		if (!std::isfinite(feature.x) || !std::isfinite(feature.y)
		    || !std::isfinite(feature.z) || !seen.insert(feature.id).second) {
			return false;
		}
	}
	for (const auto& feature : features) {
		auto* entry = Find(feature.id);
		if (!entry->visible || entry->def_id != feature.def_id) {
			AdvanceLifetime(&entry->lifetime);
		}
		entry->visible = true;
		entry->def_id = feature.def_id;
		entry->observed_state_sequence = state_sequence;
	}
	for (auto& item : entries_) {
		if (item.second.visible && seen.find(item.first) == seen.end()) {
			item.second.visible = false;
			AdvanceLifetime(&item.second.lifetime);
			item.second.observed_state_sequence = state_sequence;
		}
	}
	last_complete_state_sequence_ = state_sequence;
	return true;
}

void FeatureLifetimeLedger::MarkDestroyed(std::uint32_t id) {
	auto* entry = Find(id);
	if (entry->visible) {
		entry->visible = false;
		AdvanceLifetime(&entry->lifetime);
	}
}

std::optional<NativeFeatureReference> FeatureLifetimeLedger::Reference(
		std::uint32_t id) const {
	const auto* entry = Find(id);
	if (entry == nullptr || !entry->visible || entry->lifetime == 0) {
		return std::nullopt;
	}
	return NativeFeatureReference{id, entry->lifetime, entry->def_id,
	                              entry->observed_state_sequence};
}

bool FeatureLifetimeLedger::Matches(
		const NativeFeatureReference& reference,
		const VisibleFeatureSample& current) const {
	const auto* entry = Find(reference.id);
	return reference.lifetime != 0 && entry != nullptr && entry->visible
		&& current.id == reference.id && current.def_id == reference.def_id
		&& entry->def_id == reference.def_id
		&& entry->lifetime == reference.lifetime;
}

}  // namespace circuit::grpc
