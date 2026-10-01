// SPDX-License-Identifier: GPL-2.0-only
#include "grpc/LiveControlState.h"
#include "grpc/FactoryProductionPolicy.h"
#include "grpc/CommandQueue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

namespace circuit::grpc {

std::string LiveControlState::NewMatchIncarnation() {
	static std::atomic<std::uint64_t> counter{0};
	const auto wall = static_cast<std::uint64_t>(
		std::chrono::system_clock::now().time_since_epoch().count());
	const auto steady = static_cast<std::uint64_t>(
		std::chrono::steady_clock::now().time_since_epoch().count())
		^ (counter.fetch_add(1, std::memory_order_relaxed) + 1);
	std::string value(16, '\0');
	std::memcpy(value.data(), &wall, sizeof(wall));
	std::memcpy(value.data() + sizeof(wall), &steady, sizeof(steady));
	return value;
}

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
	if (ClassifyBasisLocked(q.live_basis) != BasisLookupResult::kKnown)
		return {false, ::highbar::v1::LIVE_FENCE_BASIS_UNKNOWN};
	const auto basis_it = bases_.find(q.live_basis.state_sequence());
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
	if (q.live_tactical_command) {
		::highbar::v1::LiveCommandBatch batch;
		*batch.mutable_actor() = q.live_actor;
		*batch.mutable_tactical_command() = *q.live_tactical_command;
		auto tactical = CheckTacticalCommandLocked(batch);
		if (!tactical.ok) return tactical;
	}
	return {true, ::highbar::v1::LIVE_FENCE_REASON_UNSPECIFIED};
}

void LiveControlState::RecordTacticalCatalogue(
		const std::string& id, std::uint64_t revision, bool complete) {
	std::lock_guard<std::mutex> lock(mutex_);
	tactical_catalogue_id_ = id;
	tactical_catalogue_revision_ = revision;
	tactical_catalogue_complete_ = complete;
	if (!complete) tactical_snapshot_.reset();
}

void LiveControlState::RecordTacticalSnapshot(
		const ::highbar::v1::TacticalSnapshotMetadata& snapshot) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (tactical_catalogue_complete_
	    && snapshot.catalogue_id() == tactical_catalogue_id_
	    && snapshot.catalogue_revision() == tactical_catalogue_revision_) {
		tactical_snapshot_ = snapshot;
	}
}

LiveFenceResult LiveControlState::CheckTacticalCommand(
		const ::highbar::v1::LiveCommandBatch& batch) const {
	std::lock_guard<std::mutex> lock(mutex_);
	return CheckTacticalCommandLocked(batch);
}

LiveFenceResult LiveControlState::CheckTacticalCommandLocked(
		const ::highbar::v1::LiveCommandBatch& batch) const {
	if (!batch.has_tactical_command())
		return {false, ::highbar::v1::LIVE_FENCE_TACTICAL_PROFILE_REQUIRED};
	const auto& command = batch.tactical_command();
	if (!tactical_catalogue_complete_ || !tactical_snapshot_)
		return {false, ::highbar::v1::LIVE_FENCE_CATALOGUE_INCOMPLETE};
	if (command.catalogue_id() != tactical_catalogue_id_
	    || command.catalogue_revision() != tactical_catalogue_revision_)
		return {false, ::highbar::v1::LIVE_FENCE_CATALOGUE_CHANGED};
	const auto actor = std::find_if(tactical_snapshot_->actors().begin(),
		tactical_snapshot_->actors().end(), [&](const auto& value) {
			return value.has_actor() && value.actor().id() == batch.actor().id()
				&& value.actor().lifetime() == batch.actor().lifetime();
		});
	if (actor == tactical_snapshot_->actors().end()
	    || actor->descriptor_revision() != command.actor_descriptor_revision())
		return {false, ::highbar::v1::LIVE_FENCE_CAPABILITY_CHANGED};
	::highbar::v1::NativeTacticalDescriptorKind required =
		::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_KIND_UNSPECIFIED;
	switch (command.action_case()) {
	case ::highbar::v1::NativeTacticalCommand::kBuild: required=::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_BUILD; break;
	case ::highbar::v1::NativeTacticalCommand::kGuard: required=::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_GUARD; break;
	case ::highbar::v1::NativeTacticalCommand::kRepair: required=::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_REPAIR; break;
	case ::highbar::v1::NativeTacticalCommand::kReclaimUnit: required=::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_RECLAIM_UNIT; break;
	case ::highbar::v1::NativeTacticalCommand::kReclaimFeature: required=::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_RECLAIM_FEATURE; break;
	case ::highbar::v1::NativeTacticalCommand::kReclaimArea: required=::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_RECLAIM_AREA; break;
	case ::highbar::v1::NativeTacticalCommand::kFactoryProduce: required=::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_FACTORY_PRODUCE; break;
	case ::highbar::v1::NativeTacticalCommand::kSetRally: required=::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_SET_RALLY; break;
	case ::highbar::v1::NativeTacticalCommand::kQueueEdit:
		required = command.queue_edit().kind()==::highbar::v1::NATIVE_QUEUE_EDIT_KIND_INSERT
			? ::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_QUEUE_INSERT
			: command.queue_edit().kind()==::highbar::v1::NATIVE_QUEUE_EDIT_KIND_REMOVE_TAG
			? ::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_QUEUE_REMOVE
			: ::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_QUEUE_REPEAT; break;
	case ::highbar::v1::NativeTacticalCommand::kTacticalMode: required=command.tactical_mode().kind(); break;
	default: return {false, ::highbar::v1::LIVE_FENCE_PARAMETER_REFUSED};
	}
	const auto descriptor=std::find_if(actor->descriptors().begin(),actor->descriptors().end(),[&](const auto& d){return d.kind()==required&&!d.disabled();});
	if(descriptor==actor->descriptors().end())return {false,::highbar::v1::LIVE_FENCE_CAPABILITY_CHANGED};
	const auto queue = std::find_if(actor->queue().begin(), actor->queue().end(),
		[&](const auto& value) { return value.domain() == command.queue_domain(); });
	if (queue == actor->queue().end() || !queue->complete()
	    || queue->revision() != command.expected_queue_revision())
		return {false, ::highbar::v1::LIVE_FENCE_QUEUE_CHANGED};
	auto owned_ref_ok=[&](const auto& ref){const auto it=owned_.find(ref.id());return ref.lifetime()!=0&&it!=owned_.end()&&it->second.present&&it->second.lifetime==ref.lifetime();};
	auto feature_ref_ok=[&](const auto& ref){return ref.lifetime()!=0&&std::any_of(tactical_snapshot_->features().begin(),tactical_snapshot_->features().end(),[&](const auto& f){return f.has_reference()&&f.reference().id()==ref.id()&&f.reference().lifetime()==ref.lifetime();});};
	auto allowed_definition=[&](std::uint32_t id){return id!=0&&std::any_of(actor->descriptors().begin(),actor->descriptors().end(),[&](const auto& value){return value.kind()==required&&!value.disabled()&&std::find(value.allowed_definition_ids().begin(),value.allowed_definition_ids().end(),id)!=value.allowed_definition_ids().end();});};
	auto actor_allows_definition=[&](std::uint32_t id){return id!=0&&std::any_of(actor->descriptors().begin(),actor->descriptors().end(),[&](const auto& value){return !value.disabled()&&std::find(value.allowed_definition_ids().begin(),value.allowed_definition_ids().end(),id)!=value.allowed_definition_ids().end();});};
	auto finite_position=[](const auto& p){return std::isfinite(p.x())&&std::isfinite(p.z())&&(!p.has_elevation()||std::isfinite(p.elevation()));};
	auto policy_ok=[&](::highbar::v1::NativeQueuePolicy policy){
		if (policy==::highbar::v1::NATIVE_QUEUE_POLICY_REPLACE||policy==::highbar::v1::NATIVE_QUEUE_POLICY_APPEND)return true;
		return policy==::highbar::v1::NATIVE_QUEUE_POLICY_REJECT_IF_BUSY&&queue->entries().empty();
	};
	if (command.action_case()==::highbar::v1::NativeTacticalCommand::kBuild
	    && (!allowed_definition(command.build().definition_id())||!finite_position(command.build().position())
	        || command.build().facing()==::highbar::v1::NATIVE_BUILD_FACING_UNSPECIFIED||!policy_ok(command.build().queue_policy())))
		return {false,::highbar::v1::LIVE_FENCE_PARAMETER_REFUSED};
	if(command.action_case()==::highbar::v1::NativeTacticalCommand::kGuard&&!owned_ref_ok(command.guard().target()))return {false,::highbar::v1::LIVE_FENCE_TARGET_NOT_FRIENDLY};
	if(command.action_case()==::highbar::v1::NativeTacticalCommand::kRepair&&!owned_ref_ok(command.repair().target()))return {false,::highbar::v1::LIVE_FENCE_TARGET_NOT_FRIENDLY};
	if(command.action_case()==::highbar::v1::NativeTacticalCommand::kReclaimUnit&&!owned_ref_ok(command.reclaim_unit().target()))return {false,::highbar::v1::LIVE_FENCE_TARGET_NOT_FRIENDLY};
	if(command.action_case()==::highbar::v1::NativeTacticalCommand::kGuard&&!policy_ok(command.guard().queue_policy()))return {false,::highbar::v1::LIVE_FENCE_PARAMETER_REFUSED};
	if(command.action_case()==::highbar::v1::NativeTacticalCommand::kRepair&&!policy_ok(command.repair().queue_policy()))return {false,::highbar::v1::LIVE_FENCE_PARAMETER_REFUSED};
	if(command.action_case()==::highbar::v1::NativeTacticalCommand::kReclaimUnit&&!policy_ok(command.reclaim_unit().queue_policy()))return {false,::highbar::v1::LIVE_FENCE_PARAMETER_REFUSED};
	if(command.action_case()==::highbar::v1::NativeTacticalCommand::kReclaimFeature){const auto& ref=command.reclaim_feature().target();if(!feature_ref_ok(ref))return {false,::highbar::v1::LIVE_FENCE_FEATURE_LIFETIME_CHANGED};if(!policy_ok(command.reclaim_feature().queue_policy()))return {false,::highbar::v1::LIVE_FENCE_PARAMETER_REFUSED};}
	if(command.action_case()==::highbar::v1::NativeTacticalCommand::kReclaimArea){const auto& body=command.reclaim_area();if(!finite_position(body.center())||!std::isfinite(body.radius_world_units())||body.radius_world_units()<=0||body.radius_world_units()>2048||!policy_ok(body.queue_policy()))return {false,::highbar::v1::LIVE_FENCE_PARAMETER_REFUSED};}
	if(command.action_case()==::highbar::v1::NativeTacticalCommand::kFactoryProduce&&
	   (command.factory_produce().count()!=1||!allowed_definition(command.factory_produce().definition_id())||
	    !FactoryProductionPolicyAllows(command.factory_produce().queue_policy(),
	                                  queue->complete(), queue->entries().empty())))
		return {false,::highbar::v1::LIVE_FENCE_PARAMETER_REFUSED};
	if(command.action_case()==::highbar::v1::NativeTacticalCommand::kSetRally
	    && !finite_position(command.set_rally().position()))
		return {false,::highbar::v1::LIVE_FENCE_PARAMETER_REFUSED};
	if(command.action_case()==::highbar::v1::NativeTacticalCommand::kTacticalMode){const auto value=command.tactical_mode().value();const int expected=required==::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_BAR_CONSTRUCTION_PRIORITY?34571:required==::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_BAR_CLOAK_DESIRE?37382:0;if(std::find(descriptor->allowed_mode_values().begin(),descriptor->allowed_mode_values().end(),value)==descriptor->allowed_mode_values().end()||!descriptor->has_native_command_id()||descriptor->native_command_id()!=expected)return {false,::highbar::v1::LIVE_FENCE_PARAMETER_REFUSED};}
	if (command.action_case() == ::highbar::v1::NativeTacticalCommand::kQueueEdit) {
		const auto& edit = command.queue_edit();
		if (edit.domain() != command.queue_domain()
		    || edit.expected_queue_revision() != command.expected_queue_revision())
			return {false, ::highbar::v1::LIVE_FENCE_QUEUE_CHANGED};
		if (edit.domain() == ::highbar::v1::NATIVE_QUEUE_DOMAIN_FACTORY_RALLY
		    && edit.kind() == ::highbar::v1::NATIVE_QUEUE_EDIT_KIND_SET_REPEAT)
			return {false, ::highbar::v1::LIVE_FENCE_PARAMETER_REFUSED};
		if (edit.kind() == ::highbar::v1::NATIVE_QUEUE_EDIT_KIND_REMOVE_TAG
		    || edit.kind() == ::highbar::v1::NATIVE_QUEUE_EDIT_KIND_INSERT) {
			const auto tag = edit.kind() == ::highbar::v1::NATIVE_QUEUE_EDIT_KIND_REMOVE_TAG
				? edit.remove_native_tag() : edit.insert().before_native_tag();
			const auto found = std::any_of(queue->entries().begin(), queue->entries().end(),
				[tag](const auto& entry) { return entry.native_tag() == tag; });
			if (!found) return {false, ::highbar::v1::LIVE_FENCE_QUEUE_TAG_CHANGED};
		}
		if (edit.kind()==::highbar::v1::NATIVE_QUEUE_EDIT_KIND_INSERT) {
			if (!edit.has_insert() || edit.insert().has_feature_target())
				return {false,::highbar::v1::LIVE_FENCE_PARAMETER_REFUSED};
			if (edit.insert().has_unit_target()&&!owned_ref_ok(edit.insert().unit_target()))
				return {false,::highbar::v1::LIVE_FENCE_TARGET_NOT_FRIENDLY};
			if (edit.insert().has_definition_id()&&!actor_allows_definition(edit.insert().definition_id()))
				return {false,::highbar::v1::LIVE_FENCE_PARAMETER_REFUSED};
			if (edit.insert().has_position()&&!finite_position(edit.insert().position()))
				return {false,::highbar::v1::LIVE_FENCE_PARAMETER_REFUSED};
		}
	}
	return {true, ::highbar::v1::LIVE_FENCE_REASON_UNSPECIFIED};
}

LiveFenceResult LiveControlState::DispatchGuarded(
		const QueuedCommand& q, const std::function<bool()>& dispatch,
		Clock::time_point now) const {
	return DispatchGuardedWithLockedState(q,
		[&](const LockedDispatchState&) { return dispatch(); }, now);
}

LiveFenceResult LiveControlState::DispatchGuardedWithLockedState(
		const QueuedCommand& q,
		const std::function<bool(const LockedDispatchState&)>& dispatch,
		Clock::time_point now) const {
	std::lock_guard<std::mutex> lock(mutex_);
	auto checked = CheckQueuedCommandLocked(q, now);
	if (!checked.ok) return checked;
	if (!dispatch(LockedDispatchState(this))) {
		return {false, ::highbar::v1::LIVE_FENCE_CAPABILITY_CHANGED};
	}
	return checked;
}

std::uint64_t LiveControlState::LockedDispatchState::OwnedLifetime(
		std::uint32_t id) const {
	return state_->OwnedLifetimeLocked(id);
}

std::uint64_t LiveControlState::OwnedLifetimeLocked(std::uint32_t id) const {
	auto it = owned_.find(id);
	return it != owned_.end() && it->second.present ? it->second.lifetime : 0;
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
	std::lock_guard<std::mutex> lock(mutex_);
	return OwnedLifetimeLocked(id);
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
	while (bases_.size() > kMaxBases) {
		auto oldest = std::min_element(bases_.begin(), bases_.end(),
			[](const auto& a, const auto& z) { return a.first < z.first; });
		bases_.erase(oldest);
	}
	return b;
}

std::optional<LiveControlState::Clock::time_point> LiveControlState::BasisExpiry(
		const ::highbar::v1::NativeObservationBasis& b) const {
	std::lock_guard<std::mutex> lock(mutex_);
	if (ClassifyBasisLocked(b) != BasisLookupResult::kKnown) return std::nullopt;
	auto it = bases_.find(b.state_sequence());
	return it->second.expires_at;
}
BasisLookupResult LiveControlState::ClassifyBasis(
		const ::highbar::v1::NativeObservationBasis& b) const {
	std::lock_guard<std::mutex> lock(mutex_);
	return ClassifyBasisLocked(b);
}
BasisLookupResult LiveControlState::ClassifyBasisLocked(
		const ::highbar::v1::NativeObservationBasis& b) const {
	auto it = bases_.find(b.state_sequence());
	if (it == bases_.end()) return BasisLookupResult::kSequenceAbsent;
	return it->second.basis.SerializeAsString() == b.SerializeAsString()
		? BasisLookupResult::kKnown : BasisLookupResult::kValueMismatch;
}
bool LiveControlState::BasisKnown(const ::highbar::v1::NativeObservationBasis& b) const {
	return ClassifyBasis(b) == BasisLookupResult::kKnown;
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
