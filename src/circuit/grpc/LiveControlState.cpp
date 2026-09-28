// SPDX-License-Identifier: GPL-2.0-only
#include "grpc/LiveControlState.h"
#include "grpc/CommandQueue.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace circuit::grpc {

LiveControlState::LiveControlState(
		std::string plugin_id, std::string process_incarnation,
		std::string match_incarnation, std::string state_channel_incarnation,
		std::string command_channel_incarnation,
		std::string control_channel_incarnation, std::size_t max_reported_units)
	: plugin_id_(std::move(plugin_id))
	, process_incarnation_(std::move(process_incarnation))
	, match_incarnation_(std::move(match_incarnation))
	, state_channel_incarnation_(std::move(state_channel_incarnation))
	, command_channel_incarnation_(std::move(command_channel_incarnation))
	, control_channel_incarnation_(std::move(control_channel_incarnation))
	, max_reported_units_(max_reported_units) {}

std::string LiveControlState::CommandChannelIncarnation() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return command_channel_incarnation_;
}

std::string LiveControlState::ControlChannelIncarnation() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return control_channel_incarnation_;
}

void LiveControlState::ReplaceChannels(
		std::string command_channel_incarnation,
		std::string control_channel_incarnation) {
	std::lock_guard<std::mutex> lock(mutex_);
	command_channel_incarnation_ = std::move(command_channel_incarnation);
	control_channel_incarnation_ = std::move(control_channel_incarnation);
	binding_.reset();
	revoked_ = true;
	control_sequence_ = 0;
	lease_deadline_ = {};
	admitted_live_batches_.clear();
}

bool LiveControlState::BindingMatchesLocal(const ::highbar::v1::LiveBinding& b) const {
	return b.plugin_id() == plugin_id_
		&& b.process_incarnation() == process_incarnation_
		&& b.match_incarnation() == match_incarnation_
		&& b.command_channel_incarnation() == command_channel_incarnation_
		&& b.control_channel_incarnation() == control_channel_incarnation_;
}

bool LiveControlState::BindingEquals(const ::highbar::v1::LiveBinding& a,
		const ::highbar::v1::LiveBinding& b) const {
	return a.SerializeAsString() == b.SerializeAsString();
}

::highbar::v1::LiveControlAckReport LiveControlState::ApplyDirective(
		const ::highbar::v1::LiveControlDirective& directive, Clock::time_point now) {
	std::lock_guard<std::mutex> lock(mutex_);
	::highbar::v1::LiveControlAckReport out;
	*out.mutable_binding() = directive.binding();
	out.set_control_sequence(directive.control_sequence());
	out.set_kind(directive.kind());
	auto refuse = [&](::highbar::v1::LiveControlAckDisposition d, const char* detail) {
		out.set_disposition(d); out.set_detail(detail); return out;
	};
	if (!BindingMatchesLocal(directive.binding())
	    || directive.binding().authority_epoch() == 0
	    || directive.control_sequence() == 0) {
		return refuse(::highbar::v1::LIVE_CONTROL_ACK_REFUSED, "binding_or_sequence_invalid");
	}
	// A locally addressed control directive means a live authority session is
	// pending even when the directive is subsequently refused. From this point
	// legacy gameplay work must not bypass the live gate.
	live_session_engaged_ = true;
	if (directive.control_sequence() == control_sequence_ && binding_
	    && BindingEquals(*binding_, directive.binding())) {
		return refuse(::highbar::v1::LIVE_CONTROL_ACK_DUPLICATE, "directive_already_applied");
	}
	if (directive.control_sequence() <= control_sequence_) {
		return refuse(::highbar::v1::LIVE_CONTROL_ACK_STALE, "control_sequence_stale");
	}
	if (binding_ && directive.binding().authority_epoch() < binding_->authority_epoch()) {
		return refuse(::highbar::v1::LIVE_CONTROL_ACK_STALE, "authority_epoch_stale");
	}
	const auto kind = directive.kind();
	if (kind == ::highbar::v1::LIVE_CONTROL_DIRECTIVE_KIND_ARM) {
		if (directive.lease_duration_ms() == 0
		    || (binding_ && directive.binding().authority_epoch() <= binding_->authority_epoch())) {
			return refuse(::highbar::v1::LIVE_CONTROL_ACK_REFUSED, "arm_requires_new_epoch_and_lease");
		}
		const auto owned = std::count_if(owned_.begin(), owned_.end(), [](const auto& p) {
			return p.second.present;
		});
		const auto targets = std::count_if(enemies_.begin(), enemies_.end(), [](const auto& p) {
			return p.second.present && p.second.visual;
		});
		if (static_cast<std::size_t>(owned + targets) > max_reported_units_) {
			revoked_ = true;
			return refuse(::highbar::v1::LIVE_CONTROL_ACK_REFUSED, "live_metadata_limit_exceeded");
		}
		binding_ = directive.binding(); revoked_ = false;
		lease_deadline_ = now + std::chrono::milliseconds(directive.lease_duration_ms());
	} else if (kind == ::highbar::v1::LIVE_CONTROL_DIRECTIVE_KIND_RENEW) {
		if (!binding_ || revoked_ || now >= lease_deadline_
		    || !BindingEquals(*binding_, directive.binding())
		    || directive.lease_duration_ms() == 0) {
			return refuse(::highbar::v1::LIVE_CONTROL_ACK_REFUSED, "renew_binding_mismatch");
		}
		lease_deadline_ = now + std::chrono::milliseconds(directive.lease_duration_ms());
	} else if (kind == ::highbar::v1::LIVE_CONTROL_DIRECTIVE_KIND_REVOKE) {
		if (!binding_ || !BindingEquals(*binding_, directive.binding())) {
			return refuse(::highbar::v1::LIVE_CONTROL_ACK_REFUSED, "revoke_binding_mismatch");
		}
		revoked_ = true;
	} else {
		return refuse(::highbar::v1::LIVE_CONTROL_ACK_REFUSED, "directive_kind_invalid");
	}
	control_sequence_ = directive.control_sequence();
	out.set_disposition(::highbar::v1::LIVE_CONTROL_ACK_RECORDED);
	out.set_detail("native_authority_gate_linearized");
	return out;
}

LiveFenceResult LiveControlState::CheckAuthority(
		const ::highbar::v1::LiveBinding& binding, Clock::time_point now) const {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!binding_ || !BindingEquals(*binding_, binding))
		return {false, ::highbar::v1::LIVE_FENCE_AUTHORITY_NOT_CONFIRMED};
	if (revoked_) return {false, ::highbar::v1::LIVE_FENCE_AUTHORITY_REVOKED};
	if (now >= lease_deadline_) return {false, ::highbar::v1::LIVE_FENCE_LEASE_EXPIRED};
	return {true, ::highbar::v1::LIVE_FENCE_REASON_UNSPECIFIED};
}

bool LiveControlState::LegacyGameplayAllowed() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return !live_session_engaged_;
}

bool LiveControlState::DispatchLegacyGuarded(
		const std::function<bool()>& dispatch) const {
	std::lock_guard<std::mutex> lock(mutex_);
	return !live_session_engaged_ && dispatch();
}

LiveFenceResult LiveControlState::CheckQueuedCommand(
		const QueuedCommand& q, Clock::time_point now) const {
	std::lock_guard<std::mutex> lock(mutex_);
	return CheckQueuedCommandLocked(q, now);
}

LiveFenceResult LiveControlState::CheckQueuedCommandLocked(
		const QueuedCommand& q, Clock::time_point now) const {
	if (!binding_ || !BindingEquals(*binding_, q.live_binding))
		return {false, ::highbar::v1::LIVE_FENCE_AUTHORITY_NOT_CONFIRMED};
	if (revoked_) return {false, ::highbar::v1::LIVE_FENCE_AUTHORITY_REVOKED};
	if (now >= lease_deadline_) return {false, ::highbar::v1::LIVE_FENCE_LEASE_EXPIRED};
	const auto basis_it = bases_.find(q.live_basis.state_sequence());
	if (basis_it == bases_.end()
	    || basis_it->second.basis.SerializeAsString() != q.live_basis.SerializeAsString())
		return {false, ::highbar::v1::LIVE_FENCE_BASIS_UNKNOWN};
	if (now >= basis_it->second.expires_at)
		return {false, ::highbar::v1::LIVE_FENCE_BASIS_EXPIRED};
	if (now >= q.live_basis_deadline) return {false, ::highbar::v1::LIVE_FENCE_BASIS_EXPIRED};
	if (now >= q.live_command_deadline) return {false, ::highbar::v1::LIVE_FENCE_COMMAND_EXPIRED};
	if (now >= q.live_lease_deadline) return {false, ::highbar::v1::LIVE_FENCE_LEASE_EXPIRED};
	const auto actor = owned_.find(q.live_actor.id());
	if (actor == owned_.end() || !actor->second.present
	    || actor->second.lifetime != q.live_actor.lifetime())
		return {false, ::highbar::v1::LIVE_FENCE_ACTOR_LIFETIME_CHANGED};
	if (q.live_semantic_action == ::highbar::v1::LIVE_SEMANTIC_ACTION_ATTACK_VISIBLE_UNIT) {
		if (!q.live_attack_target) return {false, ::highbar::v1::LIVE_FENCE_TARGET_NOT_VISUAL};
		const auto target = enemies_.find(q.live_attack_target->id());
		if (target == enemies_.end() || !target->second.present || !target->second.visual)
			return {false, ::highbar::v1::LIVE_FENCE_TARGET_NOT_VISUAL};
		if (target->second.lifetime != q.live_attack_target->lifetime())
			return {false, ::highbar::v1::LIVE_FENCE_TARGET_LIFETIME_CHANGED};
	}
	return {true, ::highbar::v1::LIVE_FENCE_REASON_UNSPECIFIED};
}

LiveFenceResult LiveControlState::DispatchGuarded(
		const QueuedCommand& q, const std::function<bool()>& dispatch,
		Clock::time_point now) const {
	std::lock_guard<std::mutex> lock(mutex_);
	auto checked = CheckQueuedCommandLocked(q, now);
	if (!checked.ok) return checked;
	if (!dispatch()) return {false, ::highbar::v1::LIVE_FENCE_CAPABILITY_CHANGED};
	return checked;
}

std::uint64_t LiveControlState::MarkOwnedPresent(std::uint32_t id) {
	std::lock_guard<std::mutex> lock(mutex_); auto& x = owned_[id];
	if (!x.present) { if (++x.lifetime == 0) ++x.lifetime; x.present = true; }
	return x.lifetime;
}
void LiveControlState::MarkOwnedRemoved(std::uint32_t id) {
	std::lock_guard<std::mutex> lock(mutex_); auto& x = owned_[id];
	if (x.present) { x.present = false; if (++x.lifetime == 0) ++x.lifetime; }
}
std::uint64_t LiveControlState::MarkEnemyPresent(std::uint32_t id, bool visual) {
	std::lock_guard<std::mutex> lock(mutex_); auto& x = enemies_[id];
	if (!x.present) { if (++x.lifetime == 0) ++x.lifetime; x.present = true; }
	x.visual = visual; return x.lifetime;
}
void LiveControlState::MarkEnemyVisual(std::uint32_t id, bool visual) {
	std::lock_guard<std::mutex> lock(mutex_); auto& x = enemies_[id];
	if (x.present) x.visual = visual;
}
void LiveControlState::MarkEnemyRemoved(std::uint32_t id) {
	std::lock_guard<std::mutex> lock(mutex_); auto& x = enemies_[id];
	if (x.present) { x.present = false; x.visual = false; if (++x.lifetime == 0) ++x.lifetime; }
}
std::uint64_t LiveControlState::OwnedLifetime(std::uint32_t id) const {
	std::lock_guard<std::mutex> lock(mutex_); auto it = owned_.find(id);
	return it != owned_.end() && it->second.present ? it->second.lifetime : 0;
}
std::uint64_t LiveControlState::EnemyLifetime(std::uint32_t id) const {
	std::lock_guard<std::mutex> lock(mutex_); auto it = enemies_.find(id);
	return it != enemies_.end() && it->second.present ? it->second.lifetime : 0;
}
bool LiveControlState::EnemyVisual(std::uint32_t id) const {
	std::lock_guard<std::mutex> lock(mutex_); auto it = enemies_.find(id);
	return it != enemies_.end() && it->second.present && it->second.visual;
}

std::string LiveControlState::BasisToken(std::uint64_t seq, std::uint64_t ns) {
	std::string out(16, '\0'); std::memcpy(out.data(), &seq, 8); std::memcpy(out.data()+8, &ns, 8); return out;
}
::highbar::v1::NativeObservationBasis LiveControlState::RecordBasis(
		std::uint64_t seq, std::uint32_t frame, std::uint64_t ns,
		std::uint32_t cadence, std::chrono::milliseconds maximum_age,
		Clock::time_point emitted_at) {
	std::lock_guard<std::mutex> lock(mutex_);
	::highbar::v1::NativeObservationBasis b; b.set_token(BasisToken(seq, ns));
	b.set_state_sequence(seq); b.set_frame(frame); b.set_match_incarnation(match_incarnation_);
	b.set_process_incarnation(process_incarnation_); b.set_state_channel_incarnation(state_channel_incarnation_);
	b.set_snapshot_send_monotonic_ns(ns); b.set_effective_cadence_frames(cadence);
	bases_[seq] = {b, emitted_at, emitted_at + maximum_age};
	while (bases_.size() > kMaxBases) bases_.erase(bases_.begin());
	return b;
}

std::optional<LiveControlState::Clock::time_point> LiveControlState::BasisExpiry(
		const ::highbar::v1::NativeObservationBasis& b) const {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = bases_.find(b.state_sequence());
	if (it == bases_.end() || it->second.basis.SerializeAsString() != b.SerializeAsString())
		return std::nullopt;
	return it->second.expires_at;
}
bool LiveControlState::BasisKnown(const ::highbar::v1::NativeObservationBasis& b) const {
	std::lock_guard<std::mutex> lock(mutex_); auto it = bases_.find(b.state_sequence());
	return it != bases_.end() && it->second.basis.SerializeAsString() == b.SerializeAsString();
}
std::optional<std::vector<::highbar::v1::NativeLiveUnitMetadata>>
LiveControlState::SnapshotUnitMetadata() {
	std::lock_guard<std::mutex> lock(mutex_);
	const auto owned_count = std::count_if(owned_.begin(), owned_.end(), [](const auto& p) {
		return p.second.present;
	});
	const auto target_count = std::count_if(enemies_.begin(), enemies_.end(), [](const auto& p) {
		return p.second.present && p.second.visual;
	});
	if (static_cast<std::size_t>(owned_count + target_count) > max_reported_units_) {
		revoked_ = true;
		return std::nullopt;
	}
	std::vector<::highbar::v1::NativeLiveUnitMetadata> out;
	auto add = [&](const auto& map, auto eligibility, bool visual_only) {
		for (const auto& [id,x] : map) { if (!x.present || (visual_only && !x.visual)) continue;
			auto& m=out.emplace_back(); m.mutable_reference()->set_id(id); m.mutable_reference()->set_lifetime(x.lifetime); m.set_eligibility(eligibility); }
	};
	add(owned_, ::highbar::v1::NATIVE_LIVE_UNIT_OWNED_ACTOR, false);
	add(enemies_, ::highbar::v1::NATIVE_LIVE_UNIT_VISUAL_TARGET, true);
	return out;
}

bool LiveControlState::LiveBatchFresh(const ::highbar::v1::LiveCommandBatch& batch) const {
	std::lock_guard<std::mutex> lock(mutex_);
	const auto& channel = batch.binding().command_channel_incarnation();
	auto it = admitted_live_batches_.find(channel);
	return it == admitted_live_batches_.end() || batch.batch().batch_seq() > it->second;
}

void LiveControlState::RememberLiveBatch(const ::highbar::v1::LiveCommandBatch& batch) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto& high = admitted_live_batches_[batch.binding().command_channel_incarnation()];
	high = std::max(high, batch.batch().batch_seq());
}

}  // namespace circuit::grpc
