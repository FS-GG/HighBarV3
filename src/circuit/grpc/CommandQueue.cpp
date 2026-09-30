// SPDX-License-Identifier: GPL-2.0-only
//
// HighBarV3 — CommandQueue impl (T055).

#include "grpc/CommandQueue.h"
#include "grpc/FactoryProductionPolicy.h"
#include "grpc/CommandDispatch.h"
#include "grpc/Counters.h"
#include "grpc/LiveControlState.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace circuit::grpc {

namespace {

const char* TacticalAdmissionDiagnosticReason(const LiveFenceResult& result) {
	switch (result.reason) {
	case ::highbar::v1::LIVE_FENCE_TACTICAL_PROFILE_REQUIRED: return "tactical_fence_profile_required";
	case ::highbar::v1::LIVE_FENCE_CATALOGUE_INCOMPLETE: return "tactical_fence_catalogue_incomplete";
	case ::highbar::v1::LIVE_FENCE_CATALOGUE_CHANGED: return "tactical_fence_catalogue_changed";
	case ::highbar::v1::LIVE_FENCE_CAPABILITY_CHANGED: return "tactical_fence_capability_changed";
	case ::highbar::v1::LIVE_FENCE_QUEUE_CHANGED: return "tactical_fence_queue_changed";
	case ::highbar::v1::LIVE_FENCE_PARAMETER_REFUSED: return "tactical_fence_parameter_refused";
	case ::highbar::v1::LIVE_FENCE_TARGET_NOT_FRIENDLY: return "tactical_fence_target_not_friendly";
	case ::highbar::v1::LIVE_FENCE_FEATURE_LIFETIME_CHANGED: return "tactical_fence_feature_lifetime_changed";
	case ::highbar::v1::LIVE_FENCE_QUEUE_TAG_CHANGED: return "tactical_fence_queue_tag_changed";
	default: return "tactical_fence_refused";
	}
}

}  // namespace

CommandQueue::CommandQueue(Counters* counters, std::size_t capacity)
	: counters_(counters), capacity_(capacity) {}

bool CommandQueue::TryPush(QueuedCommand cmd) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (queue_.size() >= capacity_) {
		return false;
	}
	queue_.push(std::move(cmd));
	if (counters_ != nullptr) {
		counters_->command_queue_depth.store(
			static_cast<std::uint32_t>(queue_.size()),
			std::memory_order_relaxed);
	}
	return true;
}

bool CommandQueue::TryPushBatch(std::vector<QueuedCommand> cmds) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (cmds.size() > capacity_ - queue_.size()) {
		return false;
	}
	for (auto& cmd : cmds) {
		queue_.push(std::move(cmd));
	}
	if (counters_ != nullptr) {
		counters_->command_queue_depth.store(
			static_cast<std::uint32_t>(queue_.size()),
			std::memory_order_relaxed);
	}
	return true;
}

std::size_t CommandQueue::Drain(std::vector<QueuedCommand>* out,
                                std::size_t max) {
	if (out == nullptr) return 0;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::size_t budget = (max == 0) ? queue_.size()
	                                      : std::min(max, queue_.size());
	out->reserve(out->size() + budget);
	for (std::size_t i = 0; i < budget; ++i) {
		out->push_back(std::move(queue_.front()));
		queue_.pop();
	}
	if (counters_ != nullptr) {
		counters_->command_queue_depth.store(
			static_cast<std::uint32_t>(queue_.size()),
			std::memory_order_relaxed);
	}
	return budget;
}

std::size_t CommandQueue::Depth() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return queue_.size();
}

std::size_t CommandQueue::AvailableCapacity() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return capacity_ - queue_.size();
}

CommandBatchResult AdmitCommandBatch(
		CommandQueue& queue,
		const ::highbar::v1::CommandBatch& batch,
		const std::string& session_id,
		const std::string& channel_incarnation) {
	constexpr int kMaxCoordinatorBatchCommands = 64;
	const int command_count = batch.commands_size();
	if (command_count == 0) {
		return {CommandBatchAdmissionStatus::kInvalidEmpty, 0};
	}
	if (command_count > kMaxCoordinatorBatchCommands) {
		return {CommandBatchAdmissionStatus::kInvalidOversized, 0};
	}
	if (batch.target_unit_id()
	    > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
		return {CommandBatchAdmissionStatus::kInvalidTarget, 0};
	}
	if (batch.batch_seq() == 0) {
		return {CommandBatchAdmissionStatus::kInvalidBatchSequence, 0};
	}
	if (!batch.has_client_command_id() || batch.client_command_id() == 0) {
		return {CommandBatchAdmissionStatus::kInvalidCorrelation, 0};
	}

	std::vector<QueuedCommand> queued;
	queued.reserve(static_cast<std::size_t>(command_count));
	for (int i = 0; i < command_count; ++i) {
		QueuedCommand child;
		child.session_id = session_id;
		child.channel_incarnation = channel_incarnation;
		child.batch_seq = batch.batch_seq();
		child.client_command_id = batch.client_command_id();
		child.command_index = static_cast<std::uint32_t>(i);
		child.authoritative_target_unit_id =
			static_cast<std::int32_t>(batch.target_unit_id());
		child.command = batch.commands(i);
		queued.push_back(std::move(child));
	}

	if (!queue.TryPushBatch(std::move(queued))) {
		return {CommandBatchAdmissionStatus::kQueueFull, 0};
	}
	return {CommandBatchAdmissionStatus::kAccepted,
	        static_cast<std::size_t>(command_count)};
}

CommandBatchResult AdmitLiveCommandBatch(
		CommandQueue& queue, const ::highbar::v1::LiveCommandBatch& live,
		const std::string& session_id, LiveControlState& state,
		std::chrono::steady_clock::time_point now) {
	const auto& batch = live.batch();
	if (batch.commands_size() != 1) return {CommandBatchAdmissionStatus::kInvalidOversized, 0};
	if (!live.has_actor() || live.actor().lifetime() == 0 || live.actor().id() > 31999u)
		return {CommandBatchAdmissionStatus::kInvalidTarget, 0, "live_actor_invalid"};
	if (batch.target_unit_id() != live.actor().id())
		return {CommandBatchAdmissionStatus::kInvalidTarget, 0, "live_target_mismatch"};
	if (!state.CheckAuthority(live.binding(), now).ok)
		return {CommandBatchAdmissionStatus::kInvalidTarget, 0, "live_authority_invalid"};
	const auto basis_lookup = state.ClassifyBasis(live.basis());
	if (basis_lookup == BasisLookupResult::kSequenceAbsent)
		return {CommandBatchAdmissionStatus::kInvalidTarget, 0, "basis_sequence_absent"};
	if (basis_lookup == BasisLookupResult::kValueMismatch)
		return {CommandBatchAdmissionStatus::kInvalidTarget, 0, "basis_value_mismatch"};
	const auto native_basis_expiry = state.BasisExpiry(live.basis());
	if (!native_basis_expiry || now >= *native_basis_expiry)
		return {CommandBatchAdmissionStatus::kInvalidTarget, 0, "live_basis_expired"};
	if (live.remaining_basis_validity_ms() == 0)
		return {CommandBatchAdmissionStatus::kInvalidTarget, 0, "live_basis_validity_zero"};
	if (live.remaining_command_lifetime_ms() == 0)
		return {CommandBatchAdmissionStatus::kInvalidTarget, 0, "live_command_lifetime_zero"};
	if (live.remaining_lease_validity_ms() == 0)
		return {CommandBatchAdmissionStatus::kInvalidTarget, 0, "live_lease_validity_zero"};
	const auto& cmd = batch.commands(0);
	bool semantic_ok = false;
	auto actor_matches = [&](std::int32_t id) {
		return id >= 0 && static_cast<std::uint32_t>(id) == live.actor().id();
	};
	auto policy_options = [](::highbar::v1::NativeQueuePolicy policy) -> std::uint32_t {
		return policy == ::highbar::v1::NATIVE_QUEUE_POLICY_APPEND ? 32u : 0u;
	};
	auto same_position = [](const ::highbar::v1::Vector3& legacy,
	                        const ::highbar::v1::NativePosition3& typed) {
		return std::isfinite(typed.x()) && std::isfinite(typed.z())
			&& (!typed.has_elevation() || std::isfinite(typed.elevation()))
			&& legacy.x() == typed.x() && legacy.z() == typed.z()
			&& legacy.y() == (typed.has_elevation() ? typed.elevation() : 0.0f);
	};
	switch (live.semantic_action()) {
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_STOP:
		semantic_ok = cmd.command_case() == ::highbar::v1::AICommand::kStop
			&& cmd.stop().options() == 0 && cmd.stop().unit_id() >= 0
			&& static_cast<std::uint32_t>(cmd.stop().unit_id()) == live.actor().id();
		break;
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_MOVE_REPLACE:
		semantic_ok = cmd.command_case() == ::highbar::v1::AICommand::kMoveUnit
			&& cmd.move_unit().options() == 0 && cmd.move_unit().unit_id() >= 0
			&& static_cast<std::uint32_t>(cmd.move_unit().unit_id()) == live.actor().id();
		break;
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_MOVE_APPEND:
		semantic_ok = cmd.command_case() == ::highbar::v1::AICommand::kMoveUnit
			&& cmd.move_unit().options() == 32u && cmd.move_unit().unit_id() >= 0
			&& static_cast<std::uint32_t>(cmd.move_unit().unit_id()) == live.actor().id();
		break;
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_ATTACK_VISIBLE_UNIT:
		semantic_ok = cmd.command_case() == ::highbar::v1::AICommand::kAttack
			&& cmd.attack().options() == 0 && live.has_visible_attack_target()
			&& cmd.attack().unit_id() >= 0
			&& static_cast<std::uint32_t>(cmd.attack().unit_id()) == live.actor().id()
			&& live.visible_attack_target().lifetime() != 0
			&& live.visible_attack_target().id() <= 31999u
			&& cmd.attack().target_unit_id() >= 0
			&& static_cast<std::uint32_t>(cmd.attack().target_unit_id()) == live.visible_attack_target().id();
		break;
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_BUILD:
		semantic_ok = live.has_tactical_command()
			&& live.tactical_command().action_case() == ::highbar::v1::NativeTacticalCommand::kBuild
			&& cmd.command_case() == ::highbar::v1::AICommand::kBuildUnit
			&& actor_matches(cmd.build_unit().unit_id())
			&& cmd.build_unit().to_build_unit_def_id()
				== static_cast<std::int32_t>(live.tactical_command().build().definition_id())
			&& cmd.build_unit().options() == policy_options(live.tactical_command().build().queue_policy())
			&& same_position(cmd.build_unit().build_position(), live.tactical_command().build().position())
			&& EngineFacingForNativeBuild(live.tactical_command().build().facing()) >= 0
			&& cmd.build_unit().facing() == EngineFacingForNativeBuild(live.tactical_command().build().facing());
		break;
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_GUARD:
		semantic_ok = live.has_tactical_command()
			&& live.tactical_command().action_case() == ::highbar::v1::NativeTacticalCommand::kGuard
			&& cmd.command_case() == ::highbar::v1::AICommand::kGuard
			&& actor_matches(cmd.guard().unit_id())
			&& cmd.guard().guard_unit_id() == static_cast<std::int32_t>(live.tactical_command().guard().target().id())
			&& cmd.guard().options() == policy_options(live.tactical_command().guard().queue_policy());
		break;
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_REPAIR:
		semantic_ok = live.has_tactical_command()
			&& live.tactical_command().action_case() == ::highbar::v1::NativeTacticalCommand::kRepair
			&& cmd.command_case() == ::highbar::v1::AICommand::kRepair
			&& actor_matches(cmd.repair().unit_id())
			&& cmd.repair().repair_unit_id() == static_cast<std::int32_t>(live.tactical_command().repair().target().id())
			&& cmd.repair().options() == policy_options(live.tactical_command().repair().queue_policy());
		break;
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_RECLAIM_UNIT:
		semantic_ok = live.has_tactical_command()
			&& live.tactical_command().action_case() == ::highbar::v1::NativeTacticalCommand::kReclaimUnit
			&& cmd.command_case() == ::highbar::v1::AICommand::kReclaimUnit
			&& actor_matches(cmd.reclaim_unit().unit_id())
			&& cmd.reclaim_unit().reclaim_unit_id() == static_cast<std::int32_t>(live.tactical_command().reclaim_unit().target().id())
			&& cmd.reclaim_unit().options() == policy_options(live.tactical_command().reclaim_unit().queue_policy());
		break;
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_RECLAIM_FEATURE:
		semantic_ok = live.has_tactical_command()
			&& live.tactical_command().action_case() == ::highbar::v1::NativeTacticalCommand::kReclaimFeature
			&& cmd.command_case() == ::highbar::v1::AICommand::kReclaimFeature
			&& actor_matches(cmd.reclaim_feature().unit_id())
			&& cmd.reclaim_feature().feature_id() == static_cast<std::int32_t>(live.tactical_command().reclaim_feature().target().id())
			&& cmd.reclaim_feature().options() == policy_options(live.tactical_command().reclaim_feature().queue_policy());
		break;
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_RECLAIM_AREA:
		semantic_ok = live.has_tactical_command()
			&& live.tactical_command().action_case() == ::highbar::v1::NativeTacticalCommand::kReclaimArea
			&& cmd.command_case() == ::highbar::v1::AICommand::kReclaimInArea
			&& actor_matches(cmd.reclaim_in_area().unit_id())
			&& cmd.reclaim_in_area().options() == policy_options(live.tactical_command().reclaim_area().queue_policy())
			&& same_position(cmd.reclaim_in_area().position(), live.tactical_command().reclaim_area().center())
			&& cmd.reclaim_in_area().radius() == live.tactical_command().reclaim_area().radius_world_units();
		break;
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_FACTORY_PRODUCE:
		semantic_ok = live.has_tactical_command()
			&& live.tactical_command().action_case() == ::highbar::v1::NativeTacticalCommand::kFactoryProduce
			&& live.tactical_command().factory_produce().count() == 1
			&& FactoryProductionPolicyAllows(
				live.tactical_command().factory_produce().queue_policy(), true, true)
			&& cmd.command_case() == ::highbar::v1::AICommand::kBuildUnit
			&& actor_matches(cmd.build_unit().unit_id())
			&& cmd.build_unit().to_build_unit_def_id() == static_cast<std::int32_t>(live.tactical_command().factory_produce().definition_id())
			&& cmd.build_unit().options() == 0;
		break;
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_SET_RALLY:
		semantic_ok = live.has_tactical_command()
			&& live.tactical_command().action_case()
				== ::highbar::v1::NativeTacticalCommand::kSetRally
			&& live.tactical_command().queue_domain()
				== ::highbar::v1::NATIVE_QUEUE_DOMAIN_FACTORY_RALLY
			&& cmd.command_case() == ::highbar::v1::AICommand::kMoveUnit
			&& actor_matches(cmd.move_unit().unit_id())
			&& cmd.move_unit().options() == 0
			&& same_position(cmd.move_unit().to_position(),
				live.tactical_command().set_rally().position());
		break;
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_QUEUE_EDIT:
		if (live.has_tactical_command()
		    && live.tactical_command().action_case() == ::highbar::v1::NativeTacticalCommand::kQueueEdit) {
			const auto& edit = live.tactical_command().queue_edit();
			const auto domain_options = edit.domain()==::highbar::v1::NATIVE_QUEUE_DOMAIN_FACTORY_PRODUCTION ? 64u : 0u;
			if (edit.kind()==::highbar::v1::NATIVE_QUEUE_EDIT_KIND_SET_REPEAT) {
				semantic_ok = cmd.command_case()==::highbar::v1::AICommand::kSetRepeat
					&& actor_matches(cmd.set_repeat().unit_id()) && cmd.set_repeat().options()==domain_options
					&& cmd.set_repeat().repeat()==edit.repeat();
			} else if (edit.kind()==::highbar::v1::NATIVE_QUEUE_EDIT_KIND_REMOVE_TAG) {
				semantic_ok = cmd.command_case()==::highbar::v1::AICommand::kCustom
					&& actor_matches(cmd.custom().unit_id()) && cmd.custom().command_id()==2
					&& cmd.custom().options()==domain_options && cmd.custom().params_size()==1
					&& cmd.custom().params(0)==static_cast<float>(edit.remove_native_tag());
			} else if (edit.kind()==::highbar::v1::NATIVE_QUEUE_EDIT_KIND_INSERT && edit.has_insert()) {
				semantic_ok = cmd.command_case()==::highbar::v1::AICommand::kCustom
					&& actor_matches(cmd.custom().unit_id()) && cmd.custom().command_id()==1
					&& cmd.custom().options()==domain_options && cmd.custom().params_size()>=3
					&& static_cast<std::int32_t>(cmd.custom().params(0))==edit.insert().before_native_tag()
					&& cmd.custom().params(0)==static_cast<float>(edit.insert().before_native_tag())
					&& cmd.custom().params(2)==0.0f;
				if (semantic_ok) {
					const auto& insert=edit.insert(); const auto legacy_id=static_cast<std::int32_t>(cmd.custom().params(1));
					switch(insert.action()) {
					case ::highbar::v1::LIVE_SEMANTIC_ACTION_MOVE_REPLACE:
						semantic_ok=legacy_id==10&&insert.has_position()&&cmd.custom().params_size()==6
							&&cmd.custom().params(3)==insert.position().x()&&cmd.custom().params(4)==(insert.position().has_elevation()?insert.position().elevation():0.0f)&&cmd.custom().params(5)==insert.position().z(); break;
					case ::highbar::v1::LIVE_SEMANTIC_ACTION_BUILD:
						semantic_ok=insert.has_definition_id()&&legacy_id==-static_cast<std::int32_t>(insert.definition_id())&&insert.has_position()&&cmd.custom().params_size()==7
							&&cmd.custom().params(3)==insert.position().x()&&cmd.custom().params(4)==(insert.position().has_elevation()?insert.position().elevation():0.0f)&&cmd.custom().params(5)==insert.position().z()&&cmd.custom().params(6)==2.0f; break;
					case ::highbar::v1::LIVE_SEMANTIC_ACTION_FACTORY_PRODUCE:
						semantic_ok=insert.has_definition_id()&&legacy_id==-static_cast<std::int32_t>(insert.definition_id())&&cmd.custom().params_size()==3; break;
					case ::highbar::v1::LIVE_SEMANTIC_ACTION_GUARD:
					case ::highbar::v1::LIVE_SEMANTIC_ACTION_REPAIR:
					case ::highbar::v1::LIVE_SEMANTIC_ACTION_RECLAIM_UNIT: {
						const int expected=insert.action()==::highbar::v1::LIVE_SEMANTIC_ACTION_GUARD?25:insert.action()==::highbar::v1::LIVE_SEMANTIC_ACTION_REPAIR?40:90;
						semantic_ok=insert.has_unit_target()&&legacy_id==expected&&cmd.custom().params_size()==4&&cmd.custom().params(3)==static_cast<float>(insert.unit_target().id()); break; }
					default: semantic_ok=false; break;
					}
				}
			}
		}
		break;
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_BAR_CONSTRUCTION_PRIORITY:
	case ::highbar::v1::LIVE_SEMANTIC_ACTION_BAR_CLOAK_DESIRE:
		semantic_ok = live.has_tactical_command()
			&& live.tactical_command().action_case() == ::highbar::v1::NativeTacticalCommand::kTacticalMode
			&& live.tactical_command().tactical_mode().kind()==(
				live.semantic_action()==::highbar::v1::LIVE_SEMANTIC_ACTION_BAR_CONSTRUCTION_PRIORITY
				? ::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_BAR_CONSTRUCTION_PRIORITY
				: ::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_BAR_CLOAK_DESIRE)
			&& cmd.command_case() == ::highbar::v1::AICommand::kCustom
			&& actor_matches(cmd.custom().unit_id()) && cmd.custom().options() == 0
			&& cmd.custom().command_id()==(live.semantic_action()==::highbar::v1::LIVE_SEMANTIC_ACTION_BAR_CONSTRUCTION_PRIORITY?34571:37382)
			&& cmd.custom().params_size()==1
			&& cmd.custom().params(0)==(live.tactical_command().tactical_mode().value()==::highbar::v1::NATIVE_TACTICAL_MODE_VALUE_ENABLED?1.0f:0.0f);
		break;
	default: break;
	}
	if (!semantic_ok)
		return {CommandBatchAdmissionStatus::kInvalidTarget, 0,
		        live.semantic_action() == ::highbar::v1::LIVE_SEMANTIC_ACTION_BUILD
		        ? "build_semantic_mismatch" : "live_semantic_mismatch"};
	if (live.has_tactical_command()) {
		const auto tactical = state.CheckTacticalCommand(live);
		if (!tactical.ok)
			return {CommandBatchAdmissionStatus::kInvalidTarget, 0,
			        TacticalAdmissionDiagnosticReason(tactical)};
	}
	if (batch.batch_seq() == 0)
		return {CommandBatchAdmissionStatus::kInvalidBatchSequence, 0};
	if (!state.LiveBatchFresh(live))
		return {CommandBatchAdmissionStatus::kDuplicate, 0};
	if (!batch.has_client_command_id() || batch.client_command_id() == 0)
		return {CommandBatchAdmissionStatus::kInvalidCorrelation, 0};
	QueuedCommand q;
	q.session_id = session_id; q.channel_incarnation = live.binding().command_channel_incarnation();
	q.batch_seq = batch.batch_seq(); q.client_command_id = batch.client_command_id(); q.command_index = 0;
	q.authoritative_target_unit_id = static_cast<std::int32_t>(live.actor().id()); q.command = cmd;
	q.live = true; q.live_binding = live.binding(); q.live_basis = live.basis(); q.live_actor = live.actor();
	if (live.has_visible_attack_target()) q.live_attack_target = live.visible_attack_target();
	if (live.has_tactical_command()) q.live_tactical_command = live.tactical_command();
	q.live_semantic_action = live.semantic_action();
	q.live_basis_deadline = std::min(
		*native_basis_expiry,
		now + std::chrono::milliseconds(live.remaining_basis_validity_ms()));
	q.live_command_deadline = now + std::chrono::milliseconds(live.remaining_command_lifetime_ms());
	q.live_lease_deadline = now + std::chrono::milliseconds(live.remaining_lease_validity_ms());
	std::vector<QueuedCommand> one; one.push_back(std::move(q));
	if (!queue.TryPushBatch(std::move(one))) return {CommandBatchAdmissionStatus::kQueueFull, 0};
	state.RememberLiveBatch(live);
	return {CommandBatchAdmissionStatus::kAccepted, 1};
}

}  // namespace circuit::grpc
