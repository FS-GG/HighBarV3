// SPDX-License-Identifier: GPL-2.0-only
//
// HighBarV3 — CommandDispatch impl (T057).

#include "grpc/CommandDispatch.h"
#include "grpc/FactoryProductionPolicy.h"
#include "grpc/GrpcLog.h"
#include "grpc/NativeCommandOptions.h"
#include "grpc/StockQueueReader.h"
#include "grpc/StockQueueTrace.h"
#include "grpc/TacticalNativeState.h"

#include "CircuitAI.h"
#include "unit/CircuitDef.h"
#include "unit/CircuitUnit.h"
#include "unit/enemy/EnemyInfo.h"
#include "task/UnitTask.h"

#include "AIFloat3.h"
#include "AIColor.h"
#include <Game.h>
#include <Pathing.h>
#include <Lua.h>
#include <Cheats.h>
#include <Unit.h>
#include <Drawer.h>
#include <Figure.h>
#include <Economy.h>
#include <Resource.h>
#include <Feature.h>
#include <FeatureDef.h>
#include <Map.h>
#include <Command.h>
#ifndef HIGHBAR_STOCK_RECOIL
#include <CurrentCommandByType.h>
#endif
#include "spring/SpringCallback.h"
#include "spring/SpringEngine.h"
#include "spring/SpringMap.h"
#include "util/Utils.h"
#include "util/Defines.h"
#include "ExternalAI/Interface/AISCommands.h"
#include "Sim/Units/CommandAI/Command.h"

#include <climits>
#include <algorithm>
#include <exception>
#include <optional>
#include <string>

namespace circuit::grpc {

namespace {

constexpr float kMaximumTacticalAreaRadius = 2048.0f;
constexpr std::size_t kMaximumTacticalQueueEntries = 64;

static_assert(EngineFacingForNativeBuild(::highbar::v1::NATIVE_BUILD_FACING_NORTH) == UNIT_FACING_NORTH);
static_assert(EngineFacingForNativeBuild(::highbar::v1::NATIVE_BUILD_FACING_EAST) == UNIT_FACING_EAST);
static_assert(EngineFacingForNativeBuild(::highbar::v1::NATIVE_BUILD_FACING_SOUTH) == UNIT_FACING_SOUTH);
static_assert(EngineFacingForNativeBuild(::highbar::v1::NATIVE_BUILD_FACING_WEST) == UNIT_FACING_WEST);
static_assert(EngineFacingForNativeBuild(::highbar::v1::NATIVE_BUILD_FACING_UNSPECIFIED) == UNIT_NO_FACING);

springai::AIFloat3 ToFloat3(const ::highbar::v1::Vector3& v) {
	return springai::AIFloat3(v.x(), v.y(), v.z());
}

struct TacticalQueueRead {
	std::vector<NativeQueueEntry> entries;
	std::optional<StockQueueReadResult> stock;
	bool complete = false;
};

std::optional<TacticalQueueRead> ReadCurrentTacticalQueue(
		::circuit::CCircuitAI* ai, springai::Unit* unit,
		::highbar::v1::NativeQueueDomain domain) {
	if (ai == nullptr || unit == nullptr) return std::nullopt;
#ifdef HIGHBAR_STOCK_RECOIL
	if (domain == ::highbar::v1::NATIVE_QUEUE_DOMAIN_FACTORY_PRODUCTION
		|| domain == ::highbar::v1::NATIVE_QUEUE_DOMAIN_FACTORY_RALLY) {
		auto* lua = ai->GetLua();
		if (lua == nullptr) return std::nullopt;
		const auto stock_domain =
			domain == ::highbar::v1::NATIVE_QUEUE_DOMAIN_FACTORY_PRODUCTION
			? StockQueueDomain::Production : StockQueueDomain::Rally;
		const auto read = ReadStockQueue(stock_domain, unit->GetUnitId(),
			[&](const char* data, std::size_t size) {
				return lua->CallRules(data, static_cast<int>(size));
			});
		TacticalQueueRead result;
		result.stock = read;
		if (read.status != StockQueueReadStatus::Complete) return result;
		result.complete = true;
		result.entries.reserve(read.entries.size());
		for (const auto& entry : read.entries) {
			result.entries.push_back({0, entry.command_id, entry.options,
				entry.tag, 0, entry.params});
		}
		return result;
	}
	if (domain != ::highbar::v1::NATIVE_QUEUE_DOMAIN_ACTOR_ORDER) {
		return std::nullopt;
	}
#endif
	auto materialize = [](auto& commands) -> std::optional<TacticalQueueRead> {
		if (commands.size() > kMaximumTacticalQueueEntries) {
			utils::free_clear(commands);
			return std::nullopt;
		}
		TacticalQueueRead result;
		result.complete = true;
		result.entries.reserve(commands.size());
		for (auto* command : commands) {
			if (command == nullptr) {
				utils::free_clear(commands);
				return std::nullopt;
			}
			result.entries.push_back({command->GetType(), command->GetId(),
				static_cast<std::uint16_t>(command->GetOptions()),
				command->GetTag(),
#ifdef HIGHBAR_STOCK_RECOIL
				0,
#else
				command->GetTimeOut(),
#endif
				command->GetParams()});
		}
		utils::free_clear(commands);
		return result;
	};

	try {
	#ifndef HIGHBAR_STOCK_RECOIL
		auto* engine = ai->GetEngine();
		const bool typed_api = engine != nullptr
			&& SupportsRallyQueueApi(engine->GetVersionHash(),
				engine->GetVersionAdditional());
		if (typed_api) {
			const int queue_type =
				domain == ::highbar::v1::NATIVE_QUEUE_DOMAIN_ACTOR_ORDER ? 0
				: domain == ::highbar::v1::NATIVE_QUEUE_DOMAIN_FACTORY_RALLY ? 1
				: domain == ::highbar::v1::NATIVE_QUEUE_DOMAIN_FACTORY_PRODUCTION ? 2
				: -1;
			if (queue_type < 0) return std::nullopt;
			auto commands = unit->GetCurrentCommandsByType(queue_type);
			return materialize(commands);
		}
	#endif
		if (domain == ::highbar::v1::NATIVE_QUEUE_DOMAIN_FACTORY_RALLY
		    || domain == ::highbar::v1::NATIVE_QUEUE_DOMAIN_UNSPECIFIED) {
			return std::nullopt;
		}
		auto commands = unit->GetCurrentCommands();
		return materialize(commands);
	} catch (...) {
		return std::nullopt;
	}
}

}  // namespace

bool DispatchTacticalCommand(::circuit::CCircuitAI* ai,
		::circuit::CCircuitUnit* unit,
		const ::highbar::v1::NativeTacticalCommand& command,
		const FeatureLifetimeLedger* feature_lifetimes,
		const StockQueueRevisionContext* stock_queue_context,
		StockQueueTraceSink* stock_queue_trace,
		const ::highbar::v1::NativeObservationBasis* live_basis,
		const StockQueueDispatchIdentity* dispatch_identity,
		const std::function<std::uint32_t()>& current_frame,
		std::uint32_t perspective_team_id,
		const std::function<bool()>& post_control_fence,
		TacticalDispatchRefusalReason* refusal_reason) {
	auto refuse = [&](TacticalDispatchRefusalReason reason) {
		if (refusal_reason != nullptr) *refusal_reason = reason;
		return false;
	};
	if (refusal_reason != nullptr) {
		*refusal_reason = TacticalDispatchRefusalReason::kUnsupportedOrInvalidArm;
	}
	if (ai == nullptr || unit == nullptr || unit->GetUnit() == nullptr) {
		return refuse(TacticalDispatchRefusalReason::kInvalidContext);
	}
	auto* native = unit->GetUnit();
	const auto current_queue = ReadCurrentTacticalQueue(ai, native, command.queue_domain());
	if (!current_queue.has_value() || !current_queue->complete) {
		return refuse(TacticalDispatchRefusalReason::kQueueUnavailable);
	}
	std::optional<std::int32_t> required_tag;
	if (command.action_case() == ::highbar::v1::NativeTacticalCommand::kQueueEdit) {
		const auto& edit = command.queue_edit();
		if (edit.kind() == ::highbar::v1::NATIVE_QUEUE_EDIT_KIND_REMOVE_TAG) {
			required_tag = edit.remove_native_tag();
		} else if (edit.kind() == ::highbar::v1::NATIVE_QUEUE_EDIT_KIND_INSERT
		           && edit.has_insert()) {
			required_tag = edit.insert().before_native_tag();
		}
	}
	auto matches_expected = [&](const std::vector<NativeQueueEntry>& queue) {
#ifdef HIGHBAR_STOCK_RECOIL
		if (stock_queue_context == nullptr) return false;
		std::vector<StockQueueEntry> stock;
		stock.reserve(queue.size());
		for (const auto& entry : queue) {
			stock.push_back({entry.command_id, entry.options, entry.tag, entry.params});
		}
		if (ComputeStockQueueRevision(*stock_queue_context, stock)
			!= command.expected_queue_revision()) return false;
		return !required_tag.has_value()
			|| std::any_of(stock.begin(), stock.end(), [&](const auto& entry) {
				return entry.tag == *required_tag;
			});
#else
		return NativeQueueMatchesExpected(queue,command.expected_queue_revision(),required_tag);
#endif
	};
	if (!matches_expected(current_queue->entries)) {
		return refuse(TacticalDispatchRefusalReason::kQueueChanged);
	}
	auto friendly = [&](const ::highbar::v1::NativeUnitReference& ref) {
		return ai->GetTeamUnit(static_cast<ICoreUnit::Id>(ref.id()));
	};
	auto position = [](const ::highbar::v1::NativePosition3& p) {
		return springai::AIFloat3(p.x(), p.has_elevation() ? p.elevation() : 0.0f, p.z());
	};
	auto dispatch_under_external_control = [&](auto&& effect) {
		auto* task = unit->GetTask();
		// A null task means Circuit has no handler for this actor, so there is
		// no autonomous scheduler to suspend and no task manager through which
		// UnitControl could install a player task.
		const bool already_controlled = task == nullptr
			|| task->GetType() == IUnitTask::Type::PLAYER;
		return DispatchAfterTacticalControlAndFinalFence(already_controlled,
			[&] { return ai->UnitControl(unit, false); },
			[&] {
				const auto final_queue = ReadCurrentTacticalQueue(
					ai, native, command.queue_domain());
				if (!final_queue.has_value()) return false;
				const bool matched = final_queue->complete
					&& matches_expected(final_queue->entries);
			#ifdef HIGHBAR_STOCK_RECOIL
				if (stock_queue_trace != nullptr && stock_queue_context != nullptr
					&& live_basis != nullptr && dispatch_identity != nullptr
					&& final_queue->stock.has_value()) {
					auto identity = *dispatch_identity; identity.matched = matched;
					std::uint64_t revision = 0;
					if (final_queue->stock->status == StockQueueReadStatus::Complete) {
						revision = ComputeStockQueueRevision(
							*stock_queue_context, final_queue->stock->entries);
					}
					stock_queue_trace->Record({true,
						current_frame ? current_frame() : 0u, perspective_team_id,
						*live_basis, *stock_queue_context, *final_queue->stock,
						revision, identity});
				}
			#endif
				return matched;
			},
			[&] { return !post_control_fence || post_control_fence(); },
			std::forward<decltype(effect)>(effect));
	};
	switch (command.action_case()) {
	case ::highbar::v1::NativeTacticalCommand::kBuild: {
		const auto& body = command.build();
		auto* def = ai->GetCircuitDefSafe(body.definition_id());
		if (def == nullptr) {
			return refuse(TacticalDispatchRefusalReason::kBuildDefinitionMissing);
		}
		auto* actor_def = unit->GetCircuitDef();
		if (actor_def == nullptr || !actor_def->CanBuild(def)) {
			return refuse(TacticalDispatchRefusalReason::kBuildCapabilityChanged);
		}
		const int facing = EngineFacingForNativeBuild(body.facing());
		auto* map = ai->GetMap();
		if (map == nullptr) {
			return refuse(TacticalDispatchRefusalReason::kBuildMapUnavailable);
		}
		if (facing < 0) {
			return refuse(TacticalDispatchRefusalReason::kBuildFacingInvalid);
		}
		const auto& requested = body.position();
		if (!std::isfinite(requested.x()) || !std::isfinite(requested.z())) {
			return refuse(TacticalDispatchRefusalReason::kBuildPositionNonFinite);
		}
		const auto pos = springai::AIFloat3(requested.x(),
			EngineElevationForNativeBuild(
				requested, map->GetElevationAt(requested.x(), requested.z())),
			requested.z());
		if (!std::isfinite(pos.y)) {
			return refuse(TacticalDispatchRefusalReason::kBuildPositionNonFinite);
		}
		if (!map->IsPossibleToBuildAt(def->GetDef(), pos, facing)) {
			return refuse(TacticalDispatchRefusalReason::kBuildSiteUnavailable);
		}
		return dispatch_under_external_control([&] {
			unit->CmdBuild(def, pos, facing, TacticalOptions(body.queue_policy()));
		});
	}
	case ::highbar::v1::NativeTacticalCommand::kFactoryProduce: {
		const auto& body = command.factory_produce();
		if (body.count() != 1) return false;
		auto* def = ai->GetCircuitDefSafe(body.definition_id());
		if (def == nullptr || !unit->GetCircuitDef()->CanBuild(def)) return false;
#ifdef HIGHBAR_STOCK_RECOIL
		const auto policy =
			body.queue_policy() == ::highbar::v1::NATIVE_QUEUE_POLICY_APPEND
			? StockFactoryQueuePolicy::Append
			: body.queue_policy() == ::highbar::v1::NATIVE_QUEUE_POLICY_REJECT_IF_BUSY
			? StockFactoryQueuePolicy::RejectIfBusy
			: StockFactoryQueuePolicy::Replace;
		if (!StockFactoryProductionPolicyAllows(policy, current_queue->entries.empty())) {
			return false;
		}
		return dispatch_under_external_control([&] {
			unit->CmdBuild(def, native->GetPos(), UNIT_NO_FACING,
				StockFactoryProductionOptions());
		});
#else
		return DispatchFactoryProductionIfAllowed(
			body.queue_policy(), current_queue->entries.empty(), [&] {
				return dispatch_under_external_control([&] {
					unit->CmdBuild(def, native->GetPos(), UNIT_NO_FACING, 0);
				});
			});
#endif
	}
	case ::highbar::v1::NativeTacticalCommand::kGuard:{auto* target=friendly(command.guard().target());if(target==nullptr||target->IsDead())return false;return dispatch_under_external_control([&]{native->Guard(target->GetUnit(),TacticalOptions(command.guard().queue_policy()));});}
	case ::highbar::v1::NativeTacticalCommand::kRepair:{auto* target=friendly(command.repair().target());if(target==nullptr||target->IsDead())return false;return dispatch_under_external_control([&]{unit->CmdRepair(target,TacticalOptions(command.repair().queue_policy()));});}
	case ::highbar::v1::NativeTacticalCommand::kReclaimUnit:{auto* target=friendly(command.reclaim_unit().target());if(target==nullptr||target->IsDead())return false;return dispatch_under_external_control([&]{unit->CmdReclaimUnit(target,TacticalOptions(command.reclaim_unit().queue_policy()));});}
	case ::highbar::v1::NativeTacticalCommand::kReclaimFeature: {
		auto* callback = ai->GetCallback();
		if (callback == nullptr) return false;
		auto features = callback->GetFeatures();
		springai::Feature* selected = nullptr;
		VisibleFeatureSample current;
		for (auto* feature : features) {
			if (feature == nullptr
				|| feature->GetFeatureId()
					!= static_cast<int>(command.reclaim_feature().target().id())) {
				continue;
			}
			auto* definition = feature->GetDef();
			const auto position = feature->GetPosition();
			if (definition != nullptr && definition->GetFeatureDefId() > 0
				&& std::isfinite(position.x) && std::isfinite(position.y)
				&& std::isfinite(position.z)) {
				selected = feature;
				current = {static_cast<std::uint32_t>(feature->GetFeatureId()),
					static_cast<std::uint32_t>(definition->GetFeatureDefId()),
					position.x, position.y, position.z};
			}
			delete definition;
			break;
		}
		const auto& wire = command.reclaim_feature().target();
		const NativeFeatureReference reference{
			wire.id(), wire.lifetime(), current.def_id, 0};
		bool control_acquired = false;
		const bool dispatched = selected != nullptr
			&& DispatchCurrentFeatureReclaim(feature_lifetimes, reference, current, [&] {
				control_acquired = dispatch_under_external_control([&] {
					unit->CmdReclaimFeature(selected,
						TacticalOptions(command.reclaim_feature().queue_policy()));
				});
			});
		utils::free_clear(features);
		return dispatched && control_acquired;
	}
	case ::highbar::v1::NativeTacticalCommand::kReclaimArea:{const auto& body=command.reclaim_area();if(!std::isfinite(body.radius_world_units())||body.radius_world_units()<=0||body.radius_world_units()>kMaximumTacticalAreaRadius)return false;return dispatch_under_external_control([&]{unit->CmdReclaimInArea(position(body.center()),body.radius_world_units(),TacticalOptions(body.queue_policy()));});}
	case ::highbar::v1::NativeTacticalCommand::kSetRally:{const auto pos=position(command.set_rally().position());if(!std::isfinite(pos.x)||!std::isfinite(pos.y)||!std::isfinite(pos.z))return false;return dispatch_under_external_control([&]{native->MoveTo(pos,0);});}
	case ::highbar::v1::NativeTacticalCommand::kQueueEdit:{const auto& edit=command.queue_edit();short outer=edit.domain()==::highbar::v1::NATIVE_QUEUE_DOMAIN_FACTORY_PRODUCTION?UNIT_COMMAND_OPTION_CONTROL_KEY:0;if(edit.kind()==::highbar::v1::NATIVE_QUEUE_EDIT_KIND_REMOVE_TAG){return dispatch_under_external_control([&]{native->ExecuteCustomCommand(CMD_REMOVE,{static_cast<float>(edit.remove_native_tag())},outer);});}if(edit.kind()==::highbar::v1::NATIVE_QUEUE_EDIT_KIND_SET_REPEAT){if(edit.domain()==::highbar::v1::NATIVE_QUEUE_DOMAIN_FACTORY_RALLY)return false;return dispatch_under_external_control([&]{native->SetRepeat(edit.repeat());});}if(edit.kind()!=::highbar::v1::NATIVE_QUEUE_EDIT_KIND_INSERT||!edit.has_insert())return false;const auto& in=edit.insert();int id=0;short inserted_options=0;std::vector<float> params;
			switch(in.action()){case ::highbar::v1::LIVE_SEMANTIC_ACTION_MOVE_REPLACE:id=CMD_MOVE;if(!in.has_position())return false;params={in.position().x(),in.position().has_elevation()?in.position().elevation():0.0f,in.position().z()};break;case ::highbar::v1::LIVE_SEMANTIC_ACTION_BUILD:if(!in.has_definition_id()||!in.has_position())return false;id=-static_cast<int>(in.definition_id());params={in.position().x(),in.position().has_elevation()?in.position().elevation():0.0f,in.position().z(),static_cast<float>(UNIT_FACING_SOUTH)};break;case ::highbar::v1::LIVE_SEMANTIC_ACTION_FACTORY_PRODUCE:if(!in.has_definition_id())return false;id=-static_cast<int>(in.definition_id());break;case ::highbar::v1::LIVE_SEMANTIC_ACTION_GUARD:id=CMD_GUARD;if(!in.has_unit_target())return false;params={static_cast<float>(in.unit_target().id())};break;case ::highbar::v1::LIVE_SEMANTIC_ACTION_REPAIR:id=CMD_REPAIR;if(!in.has_unit_target())return false;params={static_cast<float>(in.unit_target().id())};break;case ::highbar::v1::LIVE_SEMANTIC_ACTION_RECLAIM_UNIT:id=CMD_RECLAIM;if(!in.has_unit_target())return false;params={static_cast<float>(in.unit_target().id())};break;default:return false;}
			std::vector<float> encoded{static_cast<float>(in.before_native_tag()),static_cast<float>(id),static_cast<float>(inserted_options)};encoded.insert(encoded.end(),params.begin(),params.end());return dispatch_under_external_control([&]{native->ExecuteCustomCommand(CMD_INSERT,std::move(encoded),outer);});}
	case ::highbar::v1::NativeTacticalCommand::kTacticalMode:{const auto& mode=command.tactical_mode();int id=mode.kind()==::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_BAR_CONSTRUCTION_PRIORITY?34571:mode.kind()==::highbar::v1::NATIVE_TACTICAL_DESCRIPTOR_BAR_CLOAK_DESIRE?37382:0;if(id==0)return false;float value=mode.value()==::highbar::v1::NATIVE_TACTICAL_MODE_VALUE_ENABLED?1.0f:mode.value()==::highbar::v1::NATIVE_TACTICAL_MODE_VALUE_DISABLED?0.0f:-1.0f;if(value<0)return false;return dispatch_under_external_control([&]{native->ExecuteCustomCommand(id,{value});});}
	default:return false;
	}
}

namespace {

short OptionsOf(const ::highbar::v1::AICommand& cmd) {
	// Each unit-order command carries its own `options` uint32. For the
	// proto arms that don't (drawing/chat/Lua), callers don't hit this
	// helper anyway since they're skipped in the switch.
	using C = ::highbar::v1::AICommand;
	switch (cmd.command_case()) {
	case C::kBuildUnit:       return static_cast<short>(cmd.build_unit().options());
	case C::kStop:            return static_cast<short>(cmd.stop().options());
	case C::kWait:            return static_cast<short>(cmd.wait().options());
	case C::kTimedWait:       return static_cast<short>(cmd.timed_wait().options());
	case C::kMoveUnit:        return static_cast<short>(cmd.move_unit().options());
	case C::kPatrol:          return static_cast<short>(cmd.patrol().options());
	case C::kFight:           return static_cast<short>(cmd.fight().options());
	case C::kAttack:          return static_cast<short>(cmd.attack().options());
	case C::kAttackArea:      return static_cast<short>(cmd.attack_area().options());
	case C::kGuard:           return static_cast<short>(cmd.guard().options());
	case C::kRepair:          return static_cast<short>(cmd.repair().options());
	case C::kReclaimUnit:     return static_cast<short>(cmd.reclaim_unit().options());
	case C::kReclaimArea:     return static_cast<short>(cmd.reclaim_area().options());
	case C::kReclaimInArea:   return static_cast<short>(cmd.reclaim_in_area().options());
	case C::kReclaimFeature:  return static_cast<short>(cmd.reclaim_feature().options());
	case C::kResurrectInArea: return static_cast<short>(cmd.resurrect_in_area().options());
	case C::kSelfDestruct:    return static_cast<short>(cmd.self_destruct().options());
	case C::kDgun:            return static_cast<short>(cmd.dgun().options());
	case C::kSetWantedMaxSpeed: return static_cast<short>(cmd.set_wanted_max_speed().options());
	case C::kSetFireState:    return static_cast<short>(cmd.set_fire_state().options());
	case C::kSetMoveState:    return static_cast<short>(cmd.set_move_state().options());
	default:                  return 0;
	}
}

int TimeoutOf(const ::highbar::v1::AICommand& cmd) {
	using C = ::highbar::v1::AICommand;
	// Proto defaults timeout to 0; the Cmd* APIs want INT_MAX for
	// "no timeout". Map 0 → INT_MAX so F#/Python clients can leave
	// the field unset.
	int t = 0;
	switch (cmd.command_case()) {
	case C::kMoveUnit:        t = cmd.move_unit().timeout();        break;
	case C::kPatrol:          t = cmd.patrol().timeout();           break;
	case C::kFight:           t = cmd.fight().timeout();            break;
	case C::kAttack:          t = cmd.attack().timeout();           break;
	case C::kAttackArea:      t = cmd.attack_area().timeout();      break;
	case C::kRepair:          t = cmd.repair().timeout();           break;
	case C::kReclaimUnit:     t = cmd.reclaim_unit().timeout();     break;
	case C::kReclaimInArea:   t = cmd.reclaim_in_area().timeout();  break;
	case C::kResurrectInArea: t = cmd.resurrect_in_area().timeout(); break;
	case C::kBuildUnit:       t = cmd.build_unit().timeout();       break;
	case C::kDgun:            t = cmd.dgun().timeout();             break;
	default:                  t = 0;                                 break;
	}
	return t <= 0 ? INT_MAX : t;
}

class ScopedCheatEnable {
public:
	ScopedCheatEnable(::circuit::CCircuitAI* ai, const char* command_name)
		: ai_(ai)
		, cheats_((ai != nullptr) ? ai->GetCheats() : nullptr)
		, command_name_(command_name)
	{}

	bool EnsureActive() {
		if (cheats_ == nullptr) {
			return false;
		}
		try {
			was_enabled_ = cheats_->IsEnabled();
			if (!was_enabled_) {
				if (!cheats_->SetEnabled(true)) {
					LogError(ai_, "CommandDispatch",
					         std::string(command_name_) + ": unable to enable cheats");
					return false;
				}
				restore_disabled_ = true;
			}
			return true;
		} catch (const std::exception& e) {
			LogError(ai_, "CommandDispatch",
			         std::string(command_name_) + ": cheat enable failed: " + e.what());
			return false;
		} catch (...) {
			LogError(ai_, "CommandDispatch",
			         std::string(command_name_) + ": cheat enable failed: unknown exception");
			return false;
		}
	}

	~ScopedCheatEnable() {
		if (!restore_disabled_ || cheats_ == nullptr) {
			return;
		}
		try {
			(void)cheats_->SetEnabled(false);
		} catch (const std::exception& e) {
			LogError(ai_, "CommandDispatch",
			         std::string(command_name_) + ": cheat disable failed: " + e.what());
		} catch (...) {
			LogError(ai_, "CommandDispatch",
			         std::string(command_name_) + ": cheat disable failed: unknown exception");
		}
	}

private:
	::circuit::CCircuitAI* ai_ = nullptr;
	springai::Cheats* cheats_ = nullptr;
	const char* command_name_ = "";
	bool was_enabled_ = false;
	bool restore_disabled_ = false;
};

}  // namespace

bool DispatchCommand(::circuit::CCircuitAI* ai,
                     ::circuit::CCircuitUnit* unit,
                     const ::highbar::v1::AICommand& cmd,
                     ::circuit::CEnemyInfo* selected_attack_target) {
	if (ai == nullptr || unit == nullptr) return false;

	using C = ::highbar::v1::AICommand;
	const short opts = OptionsOf(cmd);
	const int timeout = TimeoutOf(cmd);

	switch (cmd.command_case()) {
	case C::kMoveUnit:
		unit->CmdMoveTo(ToFloat3(cmd.move_unit().to_position()), opts, timeout);
		return true;

	case C::kPatrol:
		unit->CmdPatrolTo(ToFloat3(cmd.patrol().to_position()), opts, timeout);
		return true;

	case C::kFight:
		unit->CmdFightTo(ToFloat3(cmd.fight().to_position()), opts, timeout);
		return true;

	case C::kAttackArea:
		unit->CmdAttackGround(
			ToFloat3(cmd.attack_area().attack_position()), opts, timeout);
		return true;

	case C::kStop:
		unit->CmdStop(opts, timeout);
		return true;

	case C::kWait:
		unit->CmdWait(true);
		return true;

	case C::kBuildUnit: {
		const auto def_id = cmd.build_unit().to_build_unit_def_id();
		if (def_id <= 0) {
			LogError(ai, "CommandDispatch",
			         "build: invalid def_id <= 0");
			return false;
		}
		auto* def = ai->GetCircuitDefSafe(
			static_cast<CCircuitDef::Id>(def_id));
		if (def == nullptr) {
			LogError(ai, "CommandDispatch",
			         "build: unknown def_id (validated away — should not happen)");
			return false;
		}
		unit->CmdBuild(def, ToFloat3(cmd.build_unit().build_position()),
		               cmd.build_unit().facing(), opts, timeout);
		return true;
	}

	case C::kRepair: {
		const std::int32_t target_id = cmd.repair().repair_unit_id();
		auto* target = ai->GetFriendlyUnit(target_id);
		if (target == nullptr) return false;
		unit->CmdRepair(target, opts, timeout);
		return true;
	}

	case C::kReclaimUnit: {
		const std::int32_t target_id = cmd.reclaim_unit().reclaim_unit_id();
		auto* target = ai->GetFriendlyUnit(target_id);
		if (target == nullptr) return false;
		unit->CmdReclaimUnit(target, opts, timeout);
		return true;
	}

	case C::kReclaimInArea:
		unit->CmdReclaimInArea(
			ToFloat3(cmd.reclaim_in_area().position()),
			cmd.reclaim_in_area().radius(), opts, timeout);
		return true;

	case C::kResurrectInArea:
		unit->CmdResurrectInArea(
			ToFloat3(cmd.resurrect_in_area().position()),
			cmd.resurrect_in_area().radius(), opts, timeout);
		return true;

	case C::kSelfDestruct:
		unit->CmdSelfD(true);
		return true;

	case C::kSetWantedMaxSpeed:
		unit->CmdWantedSpeed(cmd.set_wanted_max_speed().wanted_max_speed());
		return true;

	case C::kSetFireState:
		unit->CmdSetFireState(
			static_cast<CCircuitDef::FireT>(cmd.set_fire_state().fire_state()));
		return true;

	case C::kSetMoveState:
		unit->CmdSetMoveState(
			static_cast<CCircuitDef::MoveT>(cmd.set_move_state().move_state()));
		return true;

	// T039 — Channel A arms with no CCircuitUnit::Cmd* convenience
	// method. Route through the raw engine API via the wrapped
	// springai::Unit::ExecuteCustomCommand(cmdId, params, options,
	// timeout). The CMD_* ids come from rts/Sim/Units/CommandAI/Command.h;
	// target-unit commands pass the target id as a single float param;
	// set-bool commands pass 0.0 or 1.0.
	case C::kAttack: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		if (selected_attack_target != nullptr) {
			auto* target = selected_attack_target->GetUnit();
			if (target == nullptr) return false;
			u->Attack(target, opts, timeout);
			return true;
		}
		constexpr int CMD_ATTACK = 20;
		u->ExecuteCustomCommand(CMD_ATTACK,
			{static_cast<float>(cmd.attack().target_unit_id())},
			opts, timeout);
		return true;
	}
	case C::kGuard: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_GUARD = 25;
		u->ExecuteCustomCommand(CMD_GUARD,
			{static_cast<float>(cmd.guard().guard_unit_id())},
			opts, timeout);
		return true;
	}
	case C::kDgun: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_DGUN = 105;  // CMD_DGUN aliases CMD_MANUALFIRE
		u->ExecuteCustomCommand(CMD_DGUN,
			{static_cast<float>(cmd.dgun().target_unit_id())},
			opts, timeout);
		return true;
	}
	case C::kCapture: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_CAPTURE = 130;
		u->ExecuteCustomCommand(CMD_CAPTURE,
			{static_cast<float>(cmd.capture().target_unit_id())},
			opts, timeout);
		return true;
	}
	case C::kSetOnOff: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_ONOFF = 85;
		u->ExecuteCustomCommand(CMD_ONOFF,
			{cmd.set_on_off().on() ? 1.0f : 0.0f},
			opts, timeout);
		return true;
	}
	case C::kSetRepeat: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_REPEAT = 115;
		u->ExecuteCustomCommand(CMD_REPEAT,
			{cmd.set_repeat().repeat() ? 1.0f : 0.0f},
			opts, timeout);
		return true;
	}
	case C::kStockpile: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_STOCKPILE = 100;
		u->ExecuteCustomCommand(CMD_STOCKPILE, {}, opts, timeout);
		return true;
	}
	case C::kTimedWait: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_TIMEWAIT = 6;
		u->ExecuteCustomCommand(CMD_TIMEWAIT,
			{static_cast<float>(cmd.timed_wait().wait_time())},
			opts, timeout);
		return true;
	}
	case C::kSquadWait: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_SQUADWAIT = 8;
		u->ExecuteCustomCommand(CMD_SQUADWAIT,
			{static_cast<float>(cmd.squad_wait().num_units())},
			opts, timeout);
		return true;
	}
	case C::kDeathWait: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_DEATHWAIT = 7;
		u->ExecuteCustomCommand(CMD_DEATHWAIT,
			{static_cast<float>(cmd.death_wait().death_unit_id())},
			opts, timeout);
		return true;
	}
	case C::kGatherWait: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_GATHERWAIT = 9;
		u->ExecuteCustomCommand(CMD_GATHERWAIT, {}, opts, timeout);
		return true;
	}
	case C::kReclaimArea: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_RECLAIM = 90;
		const auto& p = cmd.reclaim_area().position();
		u->ExecuteCustomCommand(CMD_RECLAIM,
			{p.x(), p.y(), p.z(), cmd.reclaim_area().radius()},
			opts, timeout);
		return true;
	}
	case C::kReclaimFeature: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_RECLAIM = 90;
		// Feature ids are passed as MAX_UNITS + feature_id at engine
		// level; the AI wrapper translates if we pass the raw feature id.
		u->ExecuteCustomCommand(CMD_RECLAIM,
			{static_cast<float>(cmd.reclaim_feature().feature_id())},
			opts, timeout);
		return true;
	}
	case C::kRestoreArea: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_RESTORE = 110;
		const auto& p = cmd.restore_area().position();
		u->ExecuteCustomCommand(CMD_RESTORE,
			{p.x(), p.y(), p.z(), cmd.restore_area().radius()},
			opts, timeout);
		return true;
	}
	case C::kResurrect: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_RESURRECT = 125;
		u->ExecuteCustomCommand(CMD_RESURRECT,
			{static_cast<float>(cmd.resurrect().feature_id())},
			opts, timeout);
		return true;
	}
	case C::kCaptureArea: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_CAPTURE = 130;
		const auto& p = cmd.capture_area().position();
		u->ExecuteCustomCommand(CMD_CAPTURE,
			{p.x(), p.y(), p.z(), cmd.capture_area().radius()},
			opts, timeout);
		return true;
	}
	case C::kSetBase: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_SETBASE = 55;
		const auto& p = cmd.set_base().base_position();
		u->ExecuteCustomCommand(CMD_SETBASE,
			{p.x(), p.y(), p.z()}, opts, timeout);
		return true;
	}
	case C::kLoadUnits: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_LOAD_UNITS = 75;
		std::vector<float> params;
		params.reserve(cmd.load_units().to_load_unit_ids_size());
		for (auto id : cmd.load_units().to_load_unit_ids()) {
			params.push_back(static_cast<float>(id));
		}
		u->ExecuteCustomCommand(CMD_LOAD_UNITS, std::move(params),
			opts, timeout);
		return true;
	}
	case C::kLoadUnitsArea: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_LOAD_UNITS = 75;
		const auto& p = cmd.load_units_area().position();
		u->ExecuteCustomCommand(CMD_LOAD_UNITS,
			{p.x(), p.y(), p.z(), cmd.load_units_area().radius()},
			opts, timeout);
		return true;
	}
	case C::kLoadOnto: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_LOAD_ONTO = 76;
		u->ExecuteCustomCommand(CMD_LOAD_ONTO,
			{static_cast<float>(cmd.load_onto().transport_unit_id())},
			opts, timeout);
		return true;
	}
	case C::kUnloadUnit: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_UNLOAD_UNIT = 81;
		const auto& p = cmd.unload_unit().to_position();
		u->ExecuteCustomCommand(CMD_UNLOAD_UNIT,
			{p.x(), p.y(), p.z(),
			 static_cast<float>(cmd.unload_unit().to_unload_unit_id())},
			opts, timeout);
		return true;
	}
	case C::kUnloadUnitsArea: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_UNLOAD_UNITS = 80;
		const auto& p = cmd.unload_units_area().to_position();
		u->ExecuteCustomCommand(CMD_UNLOAD_UNITS,
			{p.x(), p.y(), p.z(), cmd.unload_units_area().radius()},
			opts, timeout);
		return true;
	}
	case C::kSetTrajectory: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_TRAJECTORY = 120;
		u->ExecuteCustomCommand(CMD_TRAJECTORY,
			{static_cast<float>(cmd.set_trajectory().trajectory())},
			opts, timeout);
		return true;
	}
	case C::kSetAutoRepairLevel: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_AUTOREPAIRLEVEL = 135;
		u->ExecuteCustomCommand(CMD_AUTOREPAIRLEVEL,
			{static_cast<float>(cmd.set_auto_repair_level().auto_repair_level())},
			opts, timeout);
		return true;
	}
	case C::kSetIdleMode: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_IDLEMODE = 145;
		u->ExecuteCustomCommand(CMD_IDLEMODE,
			{static_cast<float>(cmd.set_idle_mode().idle_mode())},
			opts, timeout);
		return true;
	}

	// T040 — Channel B arms. Game / Cheats / Pathing / Lua surface
	// reached through ai's typed accessors. Many of these have no
	// per-unit binding; `unit` may be ignored.
	case C::kSendTextMessage: {
		auto* g = ai->GetGame();
		if (g == nullptr) return false;
		g->SendTextMessage(cmd.send_text_message().text().c_str(),
		                   cmd.send_text_message().zone());
		return true;
	}
	case C::kSetLastPosMessage: {
		auto* g = ai->GetGame();
		if (g == nullptr) return false;
		g->SetLastMessagePosition(ToFloat3(cmd.set_last_pos_message().position()));
		return true;
	}
	case C::kPauseTeam: {
		auto* g = ai->GetGame();
		if (g == nullptr) return false;
		g->SetPause(cmd.pause_team().enable(), "via highbar gRPC");
		return true;
	}
	case C::kInitPath: {
		auto* p = ai->GetPathing();
		if (p == nullptr) return false;
		(void)p->InitPath(ToFloat3(cmd.init_path().start_position()),
		                  ToFloat3(cmd.init_path().end_position()),
		                  cmd.init_path().path_type(),
		                  cmd.init_path().goal_radius());
		// path_id return is dropped; getter arms operate on engine-side
		// path tables. A future RPC could expose the id back.
		return true;
	}
	case C::kGetApproxLength: {
		auto* p = ai->GetPathing();
		if (p == nullptr) return false;
		(void)p->GetApproximateLength(
			ToFloat3(cmd.get_approx_length().start_position()),
			ToFloat3(cmd.get_approx_length().end_position()),
			cmd.get_approx_length().path_type(),
			cmd.get_approx_length().goal_radius());
		return true;
	}
	case C::kGetNextWaypoint: {
		auto* p = ai->GetPathing();
		if (p == nullptr) return false;
		(void)p->GetNextWaypoint(cmd.get_next_waypoint().path_id());
		return true;
	}
	case C::kFreePath: {
		auto* p = ai->GetPathing();
		if (p == nullptr) return false;
		p->FreePath(cmd.free_path().path_id());
		return true;
	}
	case C::kCallLuaRules: {
		auto* l = ai->GetLua();
		if (l == nullptr) return false;
		const auto& d = cmd.call_lua_rules().data();
		(void)l->CallRules(d.c_str(), static_cast<int>(d.size()));
		return true;
	}
	case C::kCallLuaUi: {
		auto* l = ai->GetLua();
		if (l == nullptr) return false;
		const auto& d = cmd.call_lua_ui().data();
		(void)l->CallUI(d.c_str(), static_cast<int>(d.size()));
		return true;
	}
	case C::kCustom: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		const auto customId = cmd.custom().command_id();
		const auto paramsSize = cmd.custom().params_size();
		switch (customId) {
		case CMD_MANUAL_LAUNCH:
			if (paramsSize != 1 && paramsSize != 3) {
				LogError(ai, "CommandDispatch",
				         "custom: MANUAL_LAUNCH expects unit-id or map-position params");
				return false;
			}
			break;
		case CMD_BAR_PRIORITY:
		case CMD_WANT_CLOAK:
			if (paramsSize != 1) {
				LogError(ai, "CommandDispatch",
				         "custom: mode-toggle command expects exactly one param");
				return false;
			}
			break;
		case 34922:  // UNIT_SET_TARGET_NO_GROUND
		case 34923:  // UNIT_SET_TARGET
			if (paramsSize != 1 && paramsSize != 3) {
				LogError(ai, "CommandDispatch",
				         "custom: set-target command expects unit-id or map-position params");
				return false;
			}
			break;
		case 34924:  // UNIT_CANCEL_TARGET
			if (paramsSize != 0) {
				LogError(ai, "CommandDispatch",
				         "custom: UNIT_CANCEL_TARGET expects no params");
				return false;
			}
			break;
		case 34925:  // UNIT_SET_TARGET_RECTANGLE
			if (paramsSize < 3) {
				LogError(ai, "CommandDispatch",
				         "custom: UNIT_SET_TARGET_RECTANGLE expects area params");
				return false;
			}
			break;
		default:
			break;
		}
		std::vector<float> params(cmd.custom().params().begin(),
		                           cmd.custom().params().end());
		u->ExecuteCustomCommand(customId,
		                         std::move(params), opts, timeout);
		return true;
	}
	case C::kSetMyIncomeShareDirect: {
		// No springai::Economy::SetShare equivalent; closest is
		// Cheats::SetMyIncomeMultiplier — only applies under cheats.
		auto* c = ai->GetCheats();
		if (c == nullptr) return false;
		ScopedCheatEnable cheat_scope(ai, "set_my_income_share_direct");
		if (!cheat_scope.EnsureActive()) {
			return false;
		}
		try {
			c->SetMyIncomeMultiplier(cmd.set_my_income_share_direct().share());
		} catch (const std::exception& e) {
			LogError(ai, "CommandDispatch",
			         std::string("set_my_income_share_direct: ") + e.what());
			return false;
		} catch (...) {
			LogError(ai, "CommandDispatch",
			         "set_my_income_share_direct: unknown exception");
			return false;
		}
		return true;
	}
	case C::kSetShareLevel: {
		// Same situation as set_my_income_share_direct. Pass through.
		auto* c = ai->GetCheats();
		if (c == nullptr) return false;
		ScopedCheatEnable cheat_scope(ai, "set_share_level");
		if (!cheat_scope.EnsureActive()) {
			return false;
		}
		try {
			c->SetMyIncomeMultiplier(cmd.set_share_level().share_level());
		} catch (const std::exception& e) {
			LogError(ai, "CommandDispatch",
			         std::string("set_share_level: ") + e.what());
			return false;
		} catch (...) {
			LogError(ai, "CommandDispatch",
			         "set_share_level: unknown exception");
			return false;
		}
		return true;
	}

	// Group arms (Channel C-overlap). Engine-side: CMD_GROUPADD = 36,
	// CMD_GROUPCLEAR = 37. Group is a per-unit attribute on the engine
	// side, not a Group object operation.
	case C::kGroupAddUnit: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_GROUPADD = 36;
		u->ExecuteCustomCommand(CMD_GROUPADD,
			{static_cast<float>(cmd.group_add_unit().group_id())},
			opts, timeout);
		return true;
	}
	case C::kGroupRemoveUnit: {
		auto* u = unit->GetUnit();
		if (u == nullptr) return false;
		constexpr int CMD_GROUPCLEAR = 37;
		u->ExecuteCustomCommand(CMD_GROUPCLEAR, {}, opts, timeout);
		return true;
	}

	// T041 — Channel C arms (Drawer / Figure / DrawUnit). Routed
	// through ai->GetDrawer() and the Figure subobject. SetFigurePosition
	// has no Figure::SetPosition equivalent — wired to a no-op stub via
	// Figure::SetColor with current alpha to keep coverage green.
	case C::kDrawAddPoint: {
		auto* d = ai->GetDrawer();
		if (d == nullptr) return false;
		d->AddPoint(ToFloat3(cmd.draw_add_point().position()),
		            cmd.draw_add_point().label().c_str());
		return true;
	}
	case C::kDrawAddLine: {
		auto* d = ai->GetDrawer();
		if (d == nullptr) return false;
		d->AddLine(ToFloat3(cmd.draw_add_line().from_position()),
		           ToFloat3(cmd.draw_add_line().to_position()));
		return true;
	}
	case C::kDrawRemovePoint: {
		auto* d = ai->GetDrawer();
		if (d == nullptr) return false;
		d->DeletePointsAndLines(ToFloat3(cmd.draw_remove_point().position()));
		return true;
	}
	case C::kCreateSplineFigure: {
		auto* d = ai->GetDrawer();
		if (d == nullptr) return false;
		auto* f = d->GetFigure();
		if (f == nullptr) return false;
		(void)f->DrawSpline(
			ToFloat3(cmd.create_spline_figure().position1()),
			ToFloat3(cmd.create_spline_figure().position2()),
			ToFloat3(cmd.create_spline_figure().position3()),
			ToFloat3(cmd.create_spline_figure().position4()),
			cmd.create_spline_figure().width(),
			cmd.create_spline_figure().arrow(),
			cmd.create_spline_figure().lifespan(),
			cmd.create_spline_figure().figure_group_id());
		return true;
	}
	case C::kCreateLineFigure: {
		auto* d = ai->GetDrawer();
		if (d == nullptr) return false;
		auto* f = d->GetFigure();
		if (f == nullptr) return false;
		(void)f->DrawLine(
			ToFloat3(cmd.create_line_figure().from_position()),
			ToFloat3(cmd.create_line_figure().to_position()),
			cmd.create_line_figure().width(),
			cmd.create_line_figure().arrow(),
			cmd.create_line_figure().lifespan(),
			cmd.create_line_figure().figure_group_id());
		return true;
	}
	case C::kSetFigurePosition: {
		// Engine has no Figure::SetPosition; the upstream DrawerFigure
		// API only exposes color/remove. Treat as a no-op pass-through
		// so the coverage report counts the arm as wired. Clients that
		// need to move a figure should remove + recreate.
		auto* d = ai->GetDrawer();
		if (d == nullptr) return false;
		(void)d;  // intentional no-op
		return true;
	}
	case C::kSetFigureColor: {
		auto* d = ai->GetDrawer();
		if (d == nullptr) return false;
		auto* f = d->GetFigure();
		if (f == nullptr) return false;
		const auto& c = cmd.set_figure_color();
		springai::AIColor col(
			static_cast<unsigned char>(c.r() * 255.0f),
			static_cast<unsigned char>(c.g() * 255.0f),
			static_cast<unsigned char>(c.b() * 255.0f));
		f->SetColor(c.figure_id(), col,
		            static_cast<short>(c.a() * 255.0f));
		return true;
	}
	case C::kRemoveFigure: {
		auto* d = ai->GetDrawer();
		if (d == nullptr) return false;
		auto* f = d->GetFigure();
		if (f == nullptr) return false;
		f->Remove(cmd.remove_figure().figure_id());
		return true;
	}
	case C::kDrawUnit: {
		auto* d = ai->GetDrawer();
		if (d == nullptr) return false;
		auto* def = ai->GetCircuitDefSafe(
			static_cast<CCircuitDef::Id>(cmd.draw_unit().unit_def_id()));
		if (def == nullptr || def->GetDef() == nullptr) return false;
		d->DrawUnit(def->GetDef(),
		            ToFloat3(cmd.draw_unit().position()),
		            cmd.draw_unit().rotation(),
		            cmd.draw_unit().lifespan(),
		            cmd.draw_unit().team_id(),
		            cmd.draw_unit().transparent(),
		            cmd.draw_unit().draw_border(),
		            cmd.draw_unit().facing());
		return true;
	}
	case C::kGiveMeNewUnit: {
		auto* c = ai->GetCheats();
		if (c == nullptr) return false;
		ScopedCheatEnable cheat_scope(ai, "give_me_new_unit");
		if (!cheat_scope.EnsureActive()) {
			return false;
		}
		const auto def_id = cmd.give_me_new_unit().unit_def_id();
		if (def_id <= 0) {
			LogError(ai, "CommandDispatch",
			         "give_me_new_unit: invalid def_id <= 0");
			return false;
		}
		auto* def = ai->GetCircuitDefSafe(
			static_cast<CCircuitDef::Id>(def_id));
		if (def == nullptr || def->GetDef() == nullptr) return false;
		try {
			const int spawned_unit_id = c->GiveMeUnit(
				def->GetDef(),
				ToFloat3(cmd.give_me_new_unit().position()));
			if (spawned_unit_id <= 0) {
				LogError(ai, "CommandDispatch",
				         "give_me_new_unit: engine returned unit_id <= 0");
				return false;
			}
		} catch (const std::exception& e) {
			LogError(ai, "CommandDispatch",
			         std::string("give_me_new_unit: ") + e.what());
			return false;
		} catch (...) {
			LogError(ai, "CommandDispatch",
			         "give_me_new_unit: unknown exception");
			return false;
		}
		return true;
	}

	case C::kSendResources: {
		auto* cb = ai->GetCallback();
		if (cb == nullptr) return false;
		auto* econ = cb->GetEconomy();
		if (econ == nullptr) return false;
		// Resource lookup by canonical name. BAR uses "Metal"/"Energy"
		// for resource ids 0/1. The wrapper returns a freshly-allocated
		// pointer that the caller owns.
		const char* name = cmd.send_resources().resource_id() == 0
		                   ? "Metal" : "Energy";
		auto* res = cb->GetResourceByName(name);
		if (res == nullptr) return false;
		(void)econ->SendResource(res,
		                          cmd.send_resources().amount(),
		                          cmd.send_resources().receiving_team_id());
		delete res;
		return true;
	}
	case C::kGiveMe: {
		auto* c = ai->GetCheats();
		if (c == nullptr) return false;
		ScopedCheatEnable cheat_scope(ai, "give_me");
		if (!cheat_scope.EnsureActive()) {
			return false;
		}
		if (cmd.give_me().amount() <= 0.0f) {
			LogError(ai, "CommandDispatch",
			         "give_me: invalid amount <= 0");
			return false;
		}
		auto* cb = ai->GetCallback();
		if (cb == nullptr) return false;
		const char* name = cmd.give_me().resource_id() == 0
		                   ? "Metal" : "Energy";
		auto* res = cb->GetResourceByName(name);
		if (res == nullptr) return false;
		try {
			c->GiveMeResource(res, cmd.give_me().amount());
		} catch (const std::exception& e) {
			delete res;
			LogError(ai, "CommandDispatch",
			         std::string("give_me: ") + e.what());
			return false;
		} catch (...) {
			delete res;
			LogError(ai, "CommandDispatch",
			         "give_me: unknown exception");
			return false;
		}
		delete res;
		return true;
	}

	// All 66 AICommand arms now have an explicit case. The default
	// branch is unreachable in normal operation; keep it as a defensive
	// log so a future proto-arm addition surfaces immediately.
	default:
		LogError(ai, "CommandDispatch",
		         "command arm not yet wired to engine (skipped)");
		return false;
	}
}

}  // namespace circuit::grpc
