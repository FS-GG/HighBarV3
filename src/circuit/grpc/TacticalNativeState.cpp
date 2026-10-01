// SPDX-License-Identifier: GPL-2.0-only
#include "grpc/TacticalNativeState.h"

#include <algorithm>
#include <bit>
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

void AddU32Be(std::string* output, std::uint32_t value) {
	for (int shift : {24, 16, 8, 0})
		output->push_back(static_cast<char>(value >> shift));
}

void AddU64Be(std::string* output, std::uint64_t value) {
	for (int shift : {56, 48, 40, 32, 24, 16, 8, 0})
		output->push_back(static_cast<char>(value >> shift));
}

void AddField(std::string* output, unsigned char tag,
		const std::string& value) {
	output->push_back(static_cast<char>(tag));
	AddU32Be(output, static_cast<std::uint32_t>(value.size()));
	output->append(value);
}

std::string U32Bytes(std::uint32_t value) {
	std::string result;
	AddU32Be(&result, value);
	return result;
}

std::string U64Bytes(std::uint64_t value) {
	std::string result;
	AddU64Be(&result, value);
	return result;
}

bool StrictUtf8(const std::string& value) {
	// Bound identities are produced by protobuf/engine strings. Reject NUL and
	// malformed UTF-8 rather than hashing different replacement behavior.
	std::size_t index = 0;
	while (index < value.size()) {
		const auto lead = static_cast<unsigned char>(value[index]);
		if (lead == 0) return false;
		std::size_t count = 0;
		std::uint32_t code = 0;
		if (lead < 0x80) { ++index; continue; }
		if ((lead & 0xe0) == 0xc0) {
			count = 1; code = lead & 0x1f;
			if (code < 2) return false;
		} else if ((lead & 0xf0) == 0xe0) {
			count = 2; code = lead & 0x0f;
		} else if ((lead & 0xf8) == 0xf0) {
			count = 3; code = lead & 0x07;
			if (code > 4) return false;
		}
		else return false;
		if (index + count >= value.size()) return false;
		for (std::size_t offset = 1; offset <= count; ++offset) {
			const auto next = static_cast<unsigned char>(value[index + offset]);
			if ((next & 0xc0) != 0x80) return false;
			code = (code << 6) | (next & 0x3f);
		}
		if ((count == 2 && code < 0x800) || (count == 3 && code < 0x10000)
			|| code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
		index += count + 1;
	}
	return true;
}

std::string HexFloat(std::uint32_t bits) {
	static constexpr char kHex[] = "0123456789abcdef";
	std::string result(8, '0');
	for (int index = 7; index >= 0; --index) {
		result[index] = kHex[bits & 15];
		bits >>= 4;
	}
	return result;
}

bool CanonicalStockRow(const std::string& domain,
		const StockQueueEntry& entry, std::string* output) {
	if (entry.params.size() > kStockQueueMaximumParametersPerEntry) return false;
	*output = domain + "|" + std::to_string(entry.command_id) + "|"
		+ std::to_string(entry.options) + "|" + std::to_string(entry.tag) + "|";
	for (std::size_t index = 0; index < entry.params.size(); ++index) {
		if (!std::isfinite(entry.params[index])) return false;
		if (index > 0) output->push_back(',');
		output->append(HexFloat(std::bit_cast<std::uint32_t>(entry.params[index])));
	}
	return output->size() <= kStockQueueMaximumLineBytes - 4;
}

}  // namespace

bool TacticalQueueEvidenceMatches(const char* profile,
		std::uint32_t revision, QueueEvidenceScheme scheme) {
	if (profile == nullptr) return false;
	if (std::strcmp(profile, kFullTupleTacticalProfile) == 0
		&& revision == kFullTupleTacticalRevision) {
		return scheme == QueueEvidenceScheme::Unspecified
			|| scheme == QueueEvidenceScheme::FullNativeTupleV1;
	}
	return std::strcmp(profile, kStockTacticalProfile) == 0
		&& revision == kStockTacticalRevision
		&& scheme == QueueEvidenceScheme::StockLuaSupportedFieldsV1;
}

bool StockFactoryProductionPolicyAllows(StockFactoryQueuePolicy policy,
		bool observed_empty) {
	switch (policy) {
	case StockFactoryQueuePolicy::Append:
		return true;
	case StockFactoryQueuePolicy::RejectIfBusy:
		return observed_empty;
	case StockFactoryQueuePolicy::Replace:
		return false;
	}
	return false;
}

std::string BuildStockQueueRevisionPreimage(
		const StockQueueRevisionContext& context,
		const std::vector<StockQueueEntry>& entries) {
	if (context.profile != kStockTacticalProfile
		|| context.revision != kStockTacticalRevision
		|| context.evidence_scheme != QueueEvidenceScheme::StockLuaSupportedFieldsV1
		|| context.catalogue_id.empty() || context.catalogue_revision == 0
		|| context.game_content_sha256.size() != 32 || context.actor_lifetime == 0
		|| (context.domain != "actor-order" && context.domain != "production"
			&& context.domain != "rally")
		|| !StrictUtf8(context.profile) || !StrictUtf8(context.engine_version)
		|| !StrictUtf8(context.game_name) || !StrictUtf8(context.game_version)
		|| entries.size() > kStockQueueMaximumEntries) return {};
	std::vector<std::string> rows;
	rows.reserve(entries.size());
	std::size_t parameter_count = 0;
	for (const auto& entry : entries) {
		parameter_count += entry.params.size();
		if (parameter_count > kStockQueueMaximumParameters) return {};
		std::string row;
		if (!CanonicalStockRow(context.domain, entry, &row)) return {};
		rows.push_back(std::move(row));
	}
	std::string output("barc-stock-queue-revision/1\0",28);
	AddField(&output, 1, U32Bytes(static_cast<std::uint32_t>(context.evidence_scheme)));
	AddField(&output, 2, context.profile);
	AddField(&output, 3, U32Bytes(context.revision));
	AddField(&output, 4, context.catalogue_id);
	AddField(&output, 5, U64Bytes(context.catalogue_revision));
	AddField(&output, 6, context.engine_version);
	AddField(&output, 7, context.game_name);
	AddField(&output, 8, context.game_version);
	AddField(&output, 9, context.game_content_sha256);
	AddField(&output, 10, U32Bytes(context.actor_id));
	AddField(&output, 11, U64Bytes(context.actor_lifetime));
	AddField(&output, 12, context.domain);
	AddField(&output, 13, U32Bytes(static_cast<std::uint32_t>(rows.size())));
	for (const auto& row : rows) AddField(&output, 14, row);
	return output;
}

std::uint64_t ComputeStockQueueRevision(
		const StockQueueRevisionContext& context,
		const std::vector<StockQueueEntry>& entries) {
	const auto preimage = BuildStockQueueRevisionPreimage(context, entries);
	if (preimage.empty()) return 0;
	const auto digest = Sha256Digest(preimage);
	std::uint64_t revision = 0;
	for (std::size_t index = 0; index < 8; ++index) {
		revision = (revision << 8) | digest[index];
	}
	return revision == 0 ? 1 : revision;
}

bool SupportsRallyQueueApi(const char* hash, const char* additional) {
	return hash != nullptr && additional != nullptr
		&& std::strcmp(hash, kRallyQueueEngineHash) == 0
		&& std::strcmp(additional, kRallyQueueEngineAdditional) == 0;
}

bool SupportsStockRecoilProfile(const char* major, const char* minor,
		const char* patchset, const char* commits, const char* hash, const char* branch,
		const char* additional, const char* normal, const char* sync,
		const char* full, bool is_release) {
	return major != nullptr && minor != nullptr && patchset != nullptr
		&& commits != nullptr && hash != nullptr && branch != nullptr
		&& additional != nullptr
		&& normal != nullptr && sync != nullptr && full != nullptr
		&& is_release
		&& std::strcmp(major, "2025") == 0
		&& std::strcmp(minor, "06") == 0
		&& std::strcmp(patchset, "19") == 0
		&& commits[0] == '\0' && hash[0] == '\0' && branch[0] == '\0'
		&& std::strcmp(additional, "Headless") == 0
		&& std::strcmp(normal, "2025.06.19") == 0
		&& std::strcmp(sync, "2025.06.19") == 0
		&& std::strcmp(full, "2025.06.19 (Headless)") == 0;
}

std::optional<std::uint32_t> NativeBuildDefinitionId(
		std::int32_t command_id) {
	if (command_id >= 0) return std::nullopt;
	return static_cast<std::uint32_t>(-
		static_cast<std::int64_t>(command_id));
}

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

std::optional<std::uint32_t> ExactNativeQueueUnitTargetId(
		const NativeQueueEntry& entry, bool unit_target_action) {
	if (!unit_target_action || entry.params.size() != 1) return std::nullopt;
	const float value = entry.params.front();
	if (!std::isfinite(value) || value < 0.0f || value > 31999.0f
			|| std::trunc(value) != value) {
		return std::nullopt;
	}
	return static_cast<std::uint32_t>(value);
}

bool HasNativeQueueTag(const NativeQueueSnapshot& snapshot,
		std::uint64_t expected_revision, std::int32_t tag) {
	if (expected_revision == 0 || snapshot.revision != expected_revision) {
		return false;
	}
	return std::any_of(snapshot.entries.begin(), snapshot.entries.end(),
		[tag](const NativeQueueEntry& entry) { return entry.tag == tag; });
}

bool NativeQueueMatchesExpected(
		const std::vector<NativeQueueEntry>& current,
		std::uint64_t expected_revision,
		std::optional<std::int32_t> required_tag) {
	if (expected_revision == 0
	    || ComputeNativeQueueRevision(current) != expected_revision) {
		return false;
	}
	return !required_tag.has_value()
		|| std::any_of(current.begin(), current.end(), [&](const auto& entry) {
			return entry.tag == *required_tag;
		});
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

bool FeatureLifetimeLedger::ReplaceBoundedCompleteVisibleSnapshot(
		std::uint64_t state_sequence,
		std::size_t raw_count,
		const std::vector<VisibleFeatureSample>& features) {
	if (ClassifyTacticalFeaturePopulation(raw_count, features.size())
			!= TacticalFeaturePopulationStatus::Complete
		|| !ReplaceCompleteVisibleSnapshot(state_sequence, features)) {
		InvalidateVisibleSnapshot();
		return false;
	}
	return true;
}

void FeatureLifetimeLedger::InvalidateVisibleSnapshot() {
	for (auto& item : entries_) {
		if (item.second.visible) {
			item.second.visible = false;
			AdvanceLifetime(&item.second.lifetime);
		}
	}
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
