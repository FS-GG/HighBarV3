// SPDX-License-Identifier: GPL-2.0-only
//
// HighBarV3 — proto AICommand → CCircuitUnit::Cmd* dispatch (T057).
//
// Called on the engine thread from CGrpcGatewayModule::OnFrameTick
// after the CommandQueue drain. Never called from a gRPC worker
// (Constitution II — engine-thread supremacy).
//
// Coverage: the 30-ish CCircuitUnit::Cmd* methods exposed in
// unit/CircuitUnit.h (move/patrol/fight/attack-ground/stop/wait/
// build/repair/reclaim/selfd/wantedspeed/priority/fire-state/
// move-state). Proto arms without a matching Cmd* (drawing, chat,
// Lua, pathfinding, group ops, figures, cheats, stockpile, custom,
// transport load/unload) are logged and skipped — follow-up work
// wires them through the springai OOA callback layer, not through
// CCircuitUnit. The US2 Independent Test exercises MoveTo which is
// covered.

#pragma once

#include "highbar/commands.pb.h"
#include "highbar/live_control.pb.h"
#include "grpc/TacticalDispatchDiagnostic.h"

#include <cstdint>
#include <optional>

namespace circuit {
class CCircuitAI;
class CCircuitUnit;
class CEnemyInfo;
}  // namespace circuit

namespace circuit::grpc {

class FeatureLifetimeLedger;

constexpr int EngineFacingForNativeBuild(
		::highbar::v1::NativeBuildFacing facing) {
	switch (facing) {
	case ::highbar::v1::NATIVE_BUILD_FACING_NORTH: return 2;
	case ::highbar::v1::NATIVE_BUILD_FACING_EAST: return 1;
	case ::highbar::v1::NATIVE_BUILD_FACING_SOUTH: return 0;
	case ::highbar::v1::NATIVE_BUILD_FACING_WEST: return 3;
	default: return -1;
	}
}

inline bool IsGameWideCommand(const ::highbar::v1::AICommand& cmd) {
	using C = ::highbar::v1::AICommand;
	switch (cmd.command_case()) {
	case C::kSendTextMessage:
	case C::kSetLastPosMessage:
	case C::kPauseTeam:
	case C::kInitPath:
	case C::kGetApproxLength:
	case C::kGetNextWaypoint:
	case C::kFreePath:
	case C::kCallLuaRules:
	case C::kCallLuaUi:
	case C::kSetMyIncomeShareDirect:
	case C::kSetShareLevel:
	case C::kDrawAddPoint:
	case C::kDrawAddLine:
	case C::kDrawRemovePoint:
	case C::kCreateSplineFigure:
	case C::kCreateLineFigure:
	case C::kSetFigurePosition:
	case C::kSetFigureColor:
	case C::kRemoveFigure:
	case C::kDrawUnit:
	case C::kGiveMeNewUnit:
	case C::kSendResources:
	case C::kGiveMe:
		return true;
	default:
		return false;
	}
}

inline std::optional<std::int32_t> EffectiveDispatchTargetUnitId(
		std::int32_t authoritative_target_unit_id,
		const ::highbar::v1::AICommand& cmd) {
	if (IsGameWideCommand(cmd)) {
		return -1;
	}
	return authoritative_target_unit_id;
}

// Dispatch `cmd` for `unit` on the engine thread. `unit` is the live
// CCircuitUnit resolved from the batch's target_unit_id — callers
// must re-resolve before dispatch because a unit can die between
// worker-thread validation and engine-thread drain.
//
// Returns true if the command was dispatched to CCircuitUnit::Cmd*;
// false when the proto arm has no CCircuitUnit counterpart (skipped
// with a log line) or when `unit == nullptr`.
bool DispatchCommand(::circuit::CCircuitAI* ai,
                     ::circuit::CCircuitUnit* unit,
                     const ::highbar::v1::AICommand& cmd,
                     ::circuit::CEnemyInfo* selected_attack_target = nullptr);

// `refusal_reason` is meaningful only when dispatch returns false.  It feeds
// the existing opt-in coordinator trace with a fixed, payload-free reason label.
bool DispatchTacticalCommand(::circuit::CCircuitAI* ai,
	::circuit::CCircuitUnit* unit,
	const ::highbar::v1::NativeTacticalCommand& command,
	const FeatureLifetimeLedger* feature_lifetimes,
	TacticalDispatchRefusalReason* refusal_reason = nullptr);

}  // namespace circuit::grpc
