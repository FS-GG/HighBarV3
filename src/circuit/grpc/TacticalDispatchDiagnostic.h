// SPDX-License-Identifier: GPL-2.0-only
#pragma once

namespace circuit::grpc {

// Bounded reason values for the existing opt-in coordinator trace.  These
// values contain no command identifiers, positions, definitions, or payloads.
enum class TacticalDispatchRefusalReason {
	kUnsupportedOrInvalidArm,
	kInvalidContext,
	kBuildDefinitionMissing,
	kBuildCapabilityChanged,
	kBuildMapUnavailable,
	kBuildFacingInvalid,
	kBuildPositionNonFinite,
	kBuildSiteUnavailable,
};

constexpr const char* TacticalDispatchRefusalReasonName(
		TacticalDispatchRefusalReason reason) {
	switch (reason) {
	case TacticalDispatchRefusalReason::kInvalidContext:
		return "invalid_context";
	case TacticalDispatchRefusalReason::kBuildDefinitionMissing:
		return "build_definition_missing";
	case TacticalDispatchRefusalReason::kBuildCapabilityChanged:
		return "build_capability_changed";
	case TacticalDispatchRefusalReason::kBuildMapUnavailable:
		return "build_map_unavailable";
	case TacticalDispatchRefusalReason::kBuildFacingInvalid:
		return "build_facing_invalid";
	case TacticalDispatchRefusalReason::kBuildPositionNonFinite:
		return "build_position_non_finite";
	case TacticalDispatchRefusalReason::kBuildSiteUnavailable:
		return "build_site_unavailable";
	case TacticalDispatchRefusalReason::kUnsupportedOrInvalidArm:
	default:
		return "unsupported_or_invalid_arm";
	}
}

}  // namespace circuit::grpc
