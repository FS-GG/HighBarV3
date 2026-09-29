// SPDX-License-Identifier: GPL-2.0-only
//
// Keeps the legacy state stream byte-compatible while projecting complete
// world facts to the coordinator after sparse damage/destroy events.

#pragma once

#include "highbar/state.pb.h"

namespace circuit::grpc {

struct StateUpdatePlan {
	bool snapshot_before_delta = false;
	bool flush_delta = false;
	bool snapshot_after_delta = false;
};

class StateUpdateOrder {
public:
	void RequestFullStateReplacement() { replacement_pending_ = true; }
	bool ReplacementPending() const { return replacement_pending_; }

	StateUpdatePlan Plan(bool snapshot_scheduled, bool delta_pending) {
		const bool replace_delta = replacement_pending_ && snapshot_scheduled;
		if (replace_delta) replacement_pending_ = false;
		return StateUpdatePlan{
			/*snapshot_before_delta=*/snapshot_scheduled && !replace_delta,
			/*flush_delta=*/delta_pending,
			/*snapshot_after_delta=*/replace_delta,
		};
	}

private:
	bool replacement_pending_ = false;
};

// Copies the source identity and every delta arm except the two sparse enemy
// events whose wire shape cannot update current health/presence truthfully.
// Returns false when no coordinator delta remains; the following complete
// snapshot is then the coordinator's recovery baseline across the skipped seq.
inline bool BuildCoordinatorDeltaProjection(
		const ::highbar::v1::StateUpdate& source,
		::highbar::v1::StateUpdate* projection) {
	if (projection == nullptr || !source.has_delta()) return false;
	projection->Clear();
	projection->set_seq(source.seq());
	projection->set_frame(source.frame());
	projection->set_send_monotonic_ns(source.send_monotonic_ns());
	for (const auto& event : source.delta().events()) {
		if (event.kind_case() == ::highbar::v1::DeltaEvent::kEnemyDamaged
		    || event.kind_case() == ::highbar::v1::DeltaEvent::kEnemyDestroyed) {
			continue;
		}
		*projection->mutable_delta()->add_events() = event;
	}
	return projection->delta().events_size() > 0;
}

}  // namespace circuit::grpc
