// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "grpc/StockQueueReader.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace circuit::grpc {

inline constexpr const char* kFullTupleTacticalProfile =
	"barc-live-tactical-v1";
inline constexpr std::uint32_t kFullTupleTacticalRevision = 1;
inline constexpr const char* kStockTacticalProfile =
	"barc-live-tactical-stock-v1";
inline constexpr std::uint32_t kStockTacticalRevision = 2;
#ifdef HIGHBAR_STOCK_RECOIL
inline constexpr bool kStockTacticalBuild = true;
inline constexpr const char* kConfiguredTacticalProfile = kStockTacticalProfile;
inline constexpr std::uint32_t kConfiguredTacticalRevision = kStockTacticalRevision;
#else
inline constexpr bool kStockTacticalBuild = false;
inline constexpr const char* kConfiguredTacticalProfile = kFullTupleTacticalProfile;
inline constexpr std::uint32_t kConfiguredTacticalRevision = kFullTupleTacticalRevision;
#endif

enum class QueueEvidenceScheme : std::uint32_t {
	Unspecified = 0,
	FullNativeTupleV1 = 1,
	StockLuaSupportedFieldsV1 = 2,
};

// Zero retains its historical full-tuple meaning only for tactical v1.
// Stock and unknown/cross-scheme combinations fail closed.
bool TacticalQueueEvidenceMatches(const char* profile, std::uint32_t revision,
	QueueEvidenceScheme scheme);

enum class StockFactoryQueuePolicy : std::uint32_t {
	Replace = 1,
	Append = 2,
	RejectIfBusy = 3,
};

// Stock FactoryProduce Replace is unsupported even for an empty queue. There
// is no Replace-to-Append alias.
bool StockFactoryProductionPolicyAllows(StockFactoryQueuePolicy policy,
	bool observed_empty);

struct StockQueueRevisionContext {
	std::string profile;
	std::uint32_t revision = 0;
	QueueEvidenceScheme evidence_scheme = QueueEvidenceScheme::Unspecified;
	std::string catalogue_id;
	std::uint64_t catalogue_revision = 0;
	std::string engine_version;
	std::string game_name;
	std::string game_version;
	std::string game_content_sha256;
	std::uint32_t actor_id = 0;
	std::uint64_t actor_lifetime = 0;
	std::string domain;
};

// Binary, length-delimited preimage frozen by
// contracts/barc-stock-queue-v1/contract.json. Empty means the exact stock
// binding or a row is invalid; incomplete/unavailable observations never mint
// a revision.
std::string BuildStockQueueRevisionPreimage(
	const StockQueueRevisionContext& context,
	const std::vector<StockQueueEntry>& entries);
std::uint64_t ComputeStockQueueRevision(
	const StockQueueRevisionContext& context,
	const std::vector<StockQueueEntry>& entries);

// External tactical orders and Circuit's autonomous task scheduler share the
// same Spring command queue.  The ownership transition must happen before the
// accepted effect is emitted so a later autonomous update cannot insert an
// order after the final queue-revision fence.
template <typename AcquireControl, typename DispatchEffect>
bool DispatchAfterTacticalControlFence(bool already_controlled,
	AcquireControl&& acquire_control, DispatchEffect&& dispatch_effect) {
	if (!already_controlled
		&& !std::forward<AcquireControl>(acquire_control)()) {
		return false;
	}
	std::forward<DispatchEffect>(dispatch_effect)();
	return true;
}

// Stock queue evidence must be read again after Circuit has yielded control.
// Keep that final fence between control acquisition and the effect so a stale
// observation can never become an accepted command.
template <typename AcquireControl, typename FinalFence, typename DispatchEffect>
bool DispatchAfterTacticalControlAndFinalFence(bool already_controlled,
		AcquireControl&& acquire_control, FinalFence&& final_fence,
		DispatchEffect&& dispatch_effect) {
	if (!already_controlled
		&& !std::forward<AcquireControl>(acquire_control)()) {
		return false;
	}
	if (!std::forward<FinalFence>(final_fence)()) return false;
	std::forward<DispatchEffect>(dispatch_effect)();
	return true;
}

inline constexpr const char* kRallyQueueEngineHash = "7555c83";
inline constexpr const char* kRallyQueueEngineAdditional =
	"BARC-01.5-rally-api-v1 Headless";

// These values come exclusively from callbacks that predate the appended
// rally API. Callers must establish this identity before reading any appended
// SSkirmishAICallback field, because even testing such a field on an older
// callback table is out-of-bounds.
bool SupportsRallyQueueApi(const char* hash, const char* additional);

// Stock release callbacks expose no SCM hash/branch/additional suffix. This
// closed tuple prevents a custom/development artifact from negotiating the
// stock profile; the external artifact packet additionally binds the full
// executable SHA-256 because the callback ABI does not expose it.
bool SupportsStockRecoilProfile(const char* major, const char* hash,
	const char* branch, const char* additional);

// Engine-neutral projections of the Spring command and feature callbacks.
// Keeping these types independent of protobuf lets the engine thread take one
// coherent sample before a wire representation is selected.
struct NativeQueueEntry {
	std::int32_t type = 0;
	std::int32_t command_id = 0;
	std::uint16_t options = 0;
	std::int32_t tag = 0;
	std::int32_t timeout = 0;
	std::vector<float> params;
};

struct NativeQueueSnapshot {
	std::uint64_t revision = 0;
	std::vector<NativeQueueEntry> entries;
};

std::uint64_t ComputeNativeQueueRevision(
	const std::vector<NativeQueueEntry>& entries);

NativeQueueSnapshot MakeNativeQueueSnapshot(
	std::vector<NativeQueueEntry> entries);

// Spring encodes Guard and Repair unit targets as one float parameter. Return
// an id only when the caller has classified the command as a unit-targeting
// action and the callback value is an exact id in the engine's unit-id range.
std::optional<std::uint32_t> ExactNativeQueueUnitTargetId(
	const NativeQueueEntry& entry, bool unit_target_action);

struct NativeQueueUnitTarget {
	std::uint32_t id = 0;
	std::uint64_t lifetime = 0;
};

template <typename OwnedLifetime>
std::optional<NativeQueueUnitTarget> ResolveNativeQueueUnitTarget(
		const NativeQueueEntry& entry, bool unit_target_action,
		OwnedLifetime&& owned_lifetime) {
	const auto id = ExactNativeQueueUnitTargetId(entry, unit_target_action);
	if (!id.has_value()) return std::nullopt;
	const auto lifetime = std::forward<OwnedLifetime>(owned_lifetime)(*id);
	if (lifetime == 0) return std::nullopt;
	return NativeQueueUnitTarget{*id, lifetime};
}

// A tag is actionable only in the exact queue revision in which it was
// observed. Spring tags are the mutation identity; positions are not stable.
bool HasNativeQueueTag(const NativeQueueSnapshot& snapshot,
	std::uint64_t expected_revision, std::int32_t tag);

// Final engine-thread fence for effects derived from an earlier queue
// observation. The complete queue must still have the same content revision;
// tag-bearing edits additionally require that the exact native tag survives.
bool NativeQueueMatchesExpected(
	const std::vector<NativeQueueEntry>& current,
	std::uint64_t expected_revision,
	std::optional<std::int32_t> required_tag = std::nullopt);

struct VisibleFeatureSample {
	std::uint32_t id = 0;
	std::uint32_t def_id = 0;
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

struct NativeFeatureReference {
	std::uint32_t id = 0;
	std::uint64_t lifetime = 0;
	std::uint32_t def_id = 0;
	std::uint64_t observed_state_sequence = 0;
};

inline constexpr std::size_t kTacticalFeatureCapacity = 512;

enum class TacticalFeaturePopulationStatus {
	Complete,
	Overflow,
	InvalidSample,
};

inline TacticalFeaturePopulationStatus ClassifyTacticalFeaturePopulation(
		std::size_t raw_count, std::size_t valid_sample_count) {
	if (raw_count > kTacticalFeatureCapacity) {
		return TacticalFeaturePopulationStatus::Overflow;
	}
	return raw_count == valid_sample_count
		? TacticalFeaturePopulationStatus::Complete
		: TacticalFeaturePopulationStatus::InvalidSample;
}

// Feature ids can be reused and the AI callback exposes only currently visible
// features. An absence in a complete visible snapshot ends the old lifetime;
// a later observation starts a new one. A definition change is also a new
// lifetime. This is deliberately conservative for final-dispatch fencing.
class FeatureLifetimeLedger {
public:
	bool ReplaceCompleteVisibleSnapshot(
		std::uint64_t state_sequence,
		const std::vector<VisibleFeatureSample>& features);
	bool ReplaceBoundedCompleteVisibleSnapshot(
		std::uint64_t state_sequence,
		std::size_t raw_count,
		const std::vector<VisibleFeatureSample>& features);
	void InvalidateVisibleSnapshot();
	void MarkDestroyed(std::uint32_t id);

	std::optional<NativeFeatureReference> Reference(std::uint32_t id) const;
	bool Matches(const NativeFeatureReference& reference,
		const VisibleFeatureSample& current) const;

private:
	struct Entry {
		std::uint64_t lifetime = 0;
		std::uint32_t def_id = 0;
		std::uint64_t observed_state_sequence = 0;
		bool visible = false;
	};
	Entry* Find(std::uint32_t id);
	const Entry* Find(std::uint32_t id) const;
	std::vector<std::pair<std::uint32_t, Entry>> entries_;
	std::uint64_t last_complete_state_sequence_ = 0;
};

template <typename Dispatch>
bool DispatchCurrentFeatureReclaim(
		const FeatureLifetimeLedger* ledger,
		const NativeFeatureReference& reference,
		const VisibleFeatureSample& current,
		Dispatch&& dispatch) {
	if (ledger == nullptr || !ledger->Matches(reference, current)) {
		return false;
	}
	dispatch();
	return true;
}

}  // namespace circuit::grpc
