// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace circuit::grpc {

enum class TacticalSuppressionReason : std::uint8_t {
	SourceUnavailable,
	CatalogueIncomplete,
	FeatureOverflow,
	FeatureLedgerRejected,
	DescriptorOverflow,
};

enum class TacticalSuppressionSource : std::uint8_t {
	None,
	CoordinatorClient,
	LiveControlState,
	Circuit,
	Callback,
};

struct TacticalSuppressionObservation {
	TacticalSuppressionReason reason;
	TacticalSuppressionSource source = TacticalSuppressionSource::None;
	std::size_t observed = 0;
	std::size_t limit = 0;

	bool operator==(const TacticalSuppressionObservation& other) const {
		return reason == other.reason && source == other.source
			&& observed == other.observed && limit == other.limit;
	}
};

inline const char* TacticalSuppressionReasonName(TacticalSuppressionReason reason) {
	switch (reason) {
	case TacticalSuppressionReason::SourceUnavailable: return "source_unavailable";
	case TacticalSuppressionReason::CatalogueIncomplete: return "catalogue_incomplete";
	case TacticalSuppressionReason::FeatureOverflow: return "feature_overflow";
	case TacticalSuppressionReason::FeatureLedgerRejected: return "feature_ledger_rejected";
	case TacticalSuppressionReason::DescriptorOverflow: return "descriptor_overflow";
	}
	return "unknown";
}

inline const char* TacticalSuppressionSourceName(TacticalSuppressionSource source) {
	switch (source) {
	case TacticalSuppressionSource::None: return "none";
	case TacticalSuppressionSource::CoordinatorClient: return "coordinator_client";
	case TacticalSuppressionSource::LiveControlState: return "live_control_state";
	case TacticalSuppressionSource::Circuit: return "circuit";
	case TacticalSuppressionSource::Callback: return "callback";
	}
	return "unknown";
}

inline bool TacticalCountExceedsLimit(std::size_t observed, std::size_t limit) {
	return observed > limit;
}

inline std::string TacticalSuppressionTraceMessage(
		const TacticalSuppressionObservation& observation) {
	return std::string("tactical snapshot suppressed reason=")
		+ TacticalSuppressionReasonName(observation.reason)
		+ " source=" + TacticalSuppressionSourceName(observation.source)
		+ " observed=" + std::to_string(observation.observed)
		+ " limit=" + std::to_string(observation.limit);
}

// Emits the first observation, any changed observation, and one unchanged
// reminder after a bounded number of suppressed calls. The caller controls
// whether tracing is enabled; disabled tracing need not advance this state.
class TacticalSuppressionTraceThrottle {
public:
	explicit TacticalSuppressionTraceThrottle(std::size_t repeat_window)
		: repeat_window_(repeat_window == 0 ? 1 : repeat_window) {}

	bool ShouldEmit(const TacticalSuppressionObservation& observation) {
		if (!has_last_ || !(observation == last_)
			|| repeated_since_emit_ >= repeat_window_) {
			last_ = observation;
			has_last_ = true;
			repeated_since_emit_ = 0;
			return true;
		}
		++repeated_since_emit_;
		return false;
	}

	void Reset() {
		has_last_ = false;
		repeated_since_emit_ = 0;
	}

private:
	std::size_t repeat_window_;
	std::size_t repeated_since_emit_ = 0;
	bool has_last_ = false;
	TacticalSuppressionObservation last_{
		TacticalSuppressionReason::SourceUnavailable};
};

enum class CallbackFeatureVisibilityMode : std::uint8_t {
	NormalAllyTeamLos,
	CheatsAllActive,
	Unknown,
};

inline const char* CallbackFeatureVisibilityModeName(
		CallbackFeatureVisibilityMode mode) {
	switch (mode) {
	case CallbackFeatureVisibilityMode::NormalAllyTeamLos: return "normal";
	case CallbackFeatureVisibilityMode::CheatsAllActive: return "cheats";
	case CallbackFeatureVisibilityMode::Unknown: return "unknown";
	}
	return "unknown";
}

inline const char* CallbackFeatureVisibilityPolicyName(
		CallbackFeatureVisibilityMode mode) {
	switch (mode) {
	case CallbackFeatureVisibilityMode::NormalAllyTeamLos:
		return "engine_feature_is_in_los_for_allyteam";
	case CallbackFeatureVisibilityMode::CheatsAllActive:
		return "engine_all_active_features";
	case CallbackFeatureVisibilityMode::Unknown: return "unknown";
	}
	return "unknown";
}

template <typename Reader>
std::optional<CallbackFeatureVisibilityMode> ReadCallbackFeatureVisibilityMode(
		bool trace_enabled, Reader&& reader) {
	if (!trace_enabled) return std::nullopt;
	return reader();
}

struct TacticalFeatureObservation {
	CallbackFeatureVisibilityMode mode = CallbackFeatureVisibilityMode::Unknown;
	std::size_t raw = 0;
	std::optional<std::size_t> valid;
	std::size_t emitted = 0;
	bool complete = false;
	std::size_t limit = 0;

	bool operator==(const TacticalFeatureObservation& other) const {
		return mode == other.mode && raw == other.raw && valid == other.valid
			&& emitted == other.emitted && complete == other.complete
			&& limit == other.limit;
	}
};

inline std::string TacticalFeatureObservationTraceMessage(
		const TacticalFeatureObservation& observation) {
	return std::string("tactical feature observation mode=")
		+ CallbackFeatureVisibilityModeName(observation.mode)
		+ " visibility_policy="
		+ CallbackFeatureVisibilityPolicyName(observation.mode)
		+ " raw=" + std::to_string(observation.raw)
		+ " valid=" + (observation.valid.has_value()
			? std::to_string(*observation.valid) : "unknown")
		+ " emitted=" + std::to_string(observation.emitted)
		+ " complete=" + (observation.complete ? "1" : "0")
		+ " limit=" + std::to_string(observation.limit);
}

class TacticalFeatureObservationTraceState {
public:
	std::optional<std::string> MaybeMessage(
			const TacticalFeatureObservation& observation) {
		if (last_.has_value() && *last_ == observation) return std::nullopt;
		last_ = observation;
		return TacticalFeatureObservationTraceMessage(observation);
	}

private:
	std::optional<TacticalFeatureObservation> last_;
};

}  // namespace circuit::grpc
