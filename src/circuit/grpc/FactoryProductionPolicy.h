// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "highbar/live_control.pb.h"

#include <utility>

namespace circuit::grpc {

inline bool FactoryProductionPolicyAllows(
		::highbar::v1::NativeQueuePolicy policy,
		bool queue_complete, bool queue_empty) {
	if (!queue_complete) return false;
	switch (policy) {
	case ::highbar::v1::NATIVE_QUEUE_POLICY_APPEND:
		return true;
	case ::highbar::v1::NATIVE_QUEUE_POLICY_REPLACE:
	case ::highbar::v1::NATIVE_QUEUE_POLICY_REJECT_IF_BUSY:
		return queue_empty;
	default:
		return false;
	}
}

// This helper is the production dispatch gate: callers place UnitControl and
// the engine callback inside `dispatch`, so refused policies cannot acquire
// control or submit any native command.
template <typename Dispatch>
bool DispatchFactoryProductionIfAllowed(
		::highbar::v1::NativeQueuePolicy policy,
		bool current_queue_empty, Dispatch&& dispatch) {
	if (!FactoryProductionPolicyAllows(policy, true, current_queue_empty)) {
		return false;
	}
	return std::forward<Dispatch>(dispatch)();
}

}  // namespace circuit::grpc
