// SPDX-License-Identifier: GPL-2.0-only
//
// HighBarV3 — GrpcGatewayModule impl (T016, T017, T026, T030,
// plus US1 extensions T036/T037/T038/T039/T042).

#include "module/GrpcGatewayModule.h"

#include "grpc/AdminController.h"
#include "grpc/AdminService.h"
#include "grpc/AuthToken.h"
#include "grpc/CommandDispatch.h"
#include "grpc/CommandQueue.h"
#include "grpc/Config.h"
#include "grpc/CoordinatorClient.h"
#include "grpc/Counters.h"
#include "grpc/DeltaBus.h"
#include "grpc/HighBarService.h"
#include "grpc/GrpcLog.h"
#include "grpc/RingBuffer.h"
#include "grpc/OrderStateTracker.h"
#include "grpc/SchemaVersion.h"
#include "grpc/SnapshotBuilder.h"
#include "SpringHeadlessPin.h"  // T006 — kEngineReleaseId / kEngineSha256

#include "CircuitAI.h"
#include "module/EconomyManager.h"  // full def needed for OnEconomyTick
#include "scheduler/Scheduler.h"
#include "unit/CircuitUnit.h"
#include "unit/CircuitDef.h"
#include "unit/enemy/EnemyInfo.h"
#include "util/FileSystem.h"

#include "AIFloat3.h"
#include <Cheats.h>
#include <Economy.h>
#include <Game.h>
#include <Lua.h>
#include <Resource.h>
#include <Unit.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <exception>
#include <memory>
#include <mutex>
#include <future>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace circuit {

namespace {

std::mutex& CoordinatorTraceMutex() {
	static std::mutex m;
	return m;
}

void AppendCoordinatorTrace(const std::string& message) {
	const char* path = std::getenv("HIGHBAR_COORDINATOR_TRACE");
	if (path == nullptr || path[0] == '\0') return;

	std::lock_guard<std::mutex> lock(CoordinatorTraceMutex());
	FILE* f = std::fopen(path, "a");
	if (f == nullptr) return;

	const auto now = std::chrono::system_clock::now();
	const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
		now.time_since_epoch()).count();
	std::fprintf(f, "%lld plugin=module %s\n",
	             static_cast<long long>(micros),
	             message.c_str());
	std::fclose(f);
}

std::string GlobalSpeedLuaMessage(float speed) {
	std::ostringstream out;
	out << "highbar_admin_speed:" << speed;
	return out.str();
}

std::vector<std::string> GlobalSpeedAutohostCommands(float speed) {
	std::ostringstream value;
	value << speed;
	const std::string s = value.str();
	return {
		"/setmaxspeed " + s,
		"/setminspeed " + s,
		"/setmaxspeed " + s,
	};
}

bool SendAutohostCommandDatagrams(const std::vector<std::string>& commands) {
	const char* port_env = std::getenv("HIGHBAR_AUTOHOST_PORT");
	if (port_env == nullptr || port_env[0] == '\0') return false;

	char* end = nullptr;
	errno = 0;
	const long parsed_port = std::strtol(port_env, &end, 10);
	if (errno != 0 || end == port_env || *end != '\0'
	    || parsed_port <= 0 || parsed_port > 65535) {
		return false;
	}

	const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return false;

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<uint16_t>(parsed_port));
	if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
		::close(fd);
		return false;
	}

	bool ok = true;
	for (const auto& command : commands) {
		const auto sent = ::sendto(fd, command.data(), command.size(), 0,
		                           reinterpret_cast<const sockaddr*>(&addr),
		                           sizeof(addr));
		ok = ok && (sent == static_cast<ssize_t>(command.size()));
	}
	::close(fd);
	return ok;
}

std::string ResolveTokenPath(CCircuitAI* ai, const std::string& template_path) {
	if (const char* env_path = std::getenv("HIGHBAR_TOKEN_PATH")) {
		if (env_path[0] != '\0') {
			return std::string(env_path);
		}
	}
	constexpr const char* kMarker = "$writeDir/";
	const auto pos = template_path.find(kMarker);
	if (pos != 0) {
		return template_path;
	}
	std::string rel = template_path.substr(std::char_traits<char>::length(kMarker));
	if (utils::LocatePath(ai->GetCallback(), rel)) {
		return rel;
	}
	const char* tmp = std::getenv("TMPDIR");
	return (tmp != nullptr ? std::string(tmp) : std::string("/tmp"))
	       + "/highbar.token";
}

std::uint64_t NowMicros() {
	using namespace std::chrono;
	return static_cast<std::uint64_t>(
		duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

std::uint32_t FrameForWire(CCircuitAI* ai) {
	if (ai == nullptr) return 0u;
	const int frame = ai->GetLastFrame();
	return frame >= 0 ? static_cast<std::uint32_t>(frame) : 0u;
}

int CoordinatorOwnerSkirmishAiId() {
	if (const char* configured = std::getenv("HIGHBAR_COORDINATOR_OWNER_SKIRMISH_AI_ID")) {
		return std::atoi(configured);
	}
	return 0;
}

springai::AIFloat3 AdminToFloat3(const ::highbar::v1::Vector3& v) {
	return springai::AIFloat3(v.x(), v.y(), v.z());
}

const char* CommandKind(const ::highbar::v1::AICommand& cmd) {
	using C = ::highbar::v1::AICommand;
	switch (cmd.command_case()) {
	case C::kAttack:
		return "attack";
	case C::kBuildUnit:
		return "build_unit";
	case C::kMoveUnit:
		return "move_unit";
	case C::kPatrol:
		return "patrol";
	case C::kFight:
		return "fight";
	case C::kStop:
		return "stop";
	default:
		return "other";
	}
}

using CallbackRequest = ::highbar::v1::CallbackRequest;
using CallbackResponse = ::highbar::v1::CallbackResponse;
using CallbackParam = ::highbar::v1::CallbackParam;
using CallbackRpcStatus = CGrpcGatewayModule::CallbackRpcStatus;
using CallbackInvocationResult = CGrpcGatewayModule::CallbackInvocationResult;

CallbackInvocationResult MakeCallbackRpcError(
	const CallbackRequest& request,
	CallbackRpcStatus status,
	std::string detail) {
	CallbackInvocationResult result;
	result.status = status;
	result.error_detail = std::move(detail);
	result.response.set_request_id(request.request_id());
	return result;
}

CallbackInvocationResult MakeCallbackFailure(
	const CallbackRequest& request,
	std::string detail) {
	CallbackInvocationResult result;
	result.status = CallbackRpcStatus::Ok;
	result.response.set_request_id(request.request_id());
	result.response.set_success(false);
	result.response.set_error_message(std::move(detail));
	return result;
}

CallbackInvocationResult MakeCallbackSuccess(const CallbackRequest& request) {
	CallbackInvocationResult result;
	result.status = CallbackRpcStatus::Ok;
	result.response.set_request_id(request.request_id());
	result.response.set_success(true);
	return result;
}

std::optional<std::int32_t> ExtractSingleIntParam(const CallbackRequest& request,
                                                  std::string* error) {
	if (request.params_size() != 1) {
		if (error != nullptr) {
			*error = "expected exactly 1 int param";
		}
		return std::nullopt;
	}
	const auto& param = request.params(0);
	if (param.value_case() != CallbackParam::kIntValue) {
		if (error != nullptr) {
			*error = "expected int param";
		}
		return std::nullopt;
	}
	return param.int_value();
}

}  // namespace

CGrpcGatewayModule::CGrpcGatewayModule(CCircuitAI* ai)
	: IModule(ai, /*script=*/nullptr) {

	try {
		auto endpoint = grpc::LoadTransportConfig(ai);

		// 003-snapshot-arm-coverage T012 — configure the snapshot tick
		// scheduler from grpc.json. Config.cpp already validated the
		// ranges; the ctor just applies them here. The scheduler is
		// pumped from OnFrameTick once the gateway is Healthy.
		snapshot_tick_.Configure({endpoint.snapshot_tick.snapshot_cadence_frames,
		                           endpoint.snapshot_tick.snapshot_max_units});
		if (endpoint.transport == grpc::Transport::kUds) {
			socket_path_ = grpc::ResolveUdsPath(endpoint, ai);
		}

		token_file_path_ = ResolveTokenPath(ai, endpoint.ai_token_path);
		token_ = std::make_unique<grpc::AuthToken>(
			grpc::AuthToken::Generate(token_file_path_));

		// T009 — health file sits next to the token file. Write the
		// initial healthy marker so acceptance scripts can distinguish
		// "gateway never started" from "gateway started and is healthy".
		health_file_path_ = token_file_path_;
		const auto slash = health_file_path_.find_last_of('/');
		if (slash != std::string::npos) {
			health_file_path_ = health_file_path_.substr(0, slash + 1) + "highbar.health";
		} else {
			health_file_path_ = "highbar.health";
		}
		grpc::WriteHealthFile(health_file_path_, /*healthy=*/true);

		counters_ = std::make_unique<grpc::Counters>();

		// US1 pieces.
		snapshot_ = std::make_unique<grpc::SnapshotBuilder>(ai);
		delta_bus_ = std::make_unique<grpc::DeltaBus>(counters_.get());
		ring_ = std::make_unique<grpc::RingBuffer>(endpoint.ring_size);

		// US2 pieces (T055 + T057 command path).
		command_queue_ = std::make_unique<grpc::CommandQueue>(counters_.get());
		order_state_tracker_ = std::make_unique<grpc::OrderStateTracker>();
		admin_controller_ = std::make_unique<grpc::AdminController>();
		admin_service_ = std::make_unique<grpc::AdminService>(
			admin_controller_.get());
		admin_service_->SetClock(
			[this]() { return this->CurrentFrame(); },
			[this]() { return this->HeadSeq(); });
		admin_service_->SetExecutionFn(
			[this](const grpc::AdminCaller& caller,
			       const ::highbar::v1::AdminAction& action,
			       std::chrono::milliseconds timeout) {
				return this->QueueAdminAction(caller, action, timeout);
			});

		service_ = std::make_unique<grpc::HighBarService>(
			ai, counters_.get(), token_.get());
		// HighBarService needs the US1 handles. Public setters wire
		// them post-construction so the ctor stays non-failing for
		// environments where the gateway runs without a full US1
		// implementation (see HighBarService::SetUs1Handles).
		service_->SetUs1Handles(snapshot_.get(), delta_bus_.get(),
		                        ring_.get(), &state_mutex_);
		service_->SetUs2Handles(command_queue_.get());
		service_->SetAdminService(admin_service_.get());
		// 003-snapshot-arm-coverage — RequestSnapshot handler needs the
		// module to flip the pending atomic + read the current frame.
		service_->SetSnapshotHandle(this);
		// T013 — worker-thread fault requests from service handlers
		// route here and are applied on the engine thread next tick.
		service_->SetFaultSink([this](const std::string& s,
		                               const std::string& r,
		                               const std::string& d) {
			this->RequestDisable(s, r, d);
		});
		const bool client_mode_enabled =
			(std::getenv("HIGHBAR_COORDINATOR") != nullptr);
		const std::string transport_name =
			endpoint.transport == grpc::Transport::kUds ? "uds" : "tcp";
		if (client_mode_enabled) {
			deferred_service_bind_endpoint_ = endpoint;
			bound_address_ = "client-mode-deferred-bind";
		} else {
			service_->Bind(endpoint, &bound_address_);
			service_bound_ = true;
		}

		// T023 — startup banner now includes engine pin so the pin is
		// observable in the log alongside transport and schema.
		std::string short_sha = grpc::kEngineSha256;
		if (short_sha.size() > 12) short_sha = short_sha.substr(0, 12);
		grpc::LogStartup(ai, client_mode_enabled ? "client-mode" : transport_name, bound_address_,
		                 std::string(::highbar::v1::kSchemaVersion)
		                 + " engine=" + grpc::kEngineReleaseId
		                 + " sha256=" + short_sha);

		// Client-mode coordinator dial. Endpoint is configurable via env
		// var HIGHBAR_COORDINATOR (e.g., "unix:/tmp/hb-coord.sock" or
		// "127.0.0.1:50600"). If unset, client-mode is disabled and the
		// plugin behaves as before (server-mode only — currently broken
		// inside spring; see investigations/hello-rpc-deadline-exceeded.md).
		if (const char* coord_env = std::getenv("HIGHBAR_COORDINATOR")) {
			const std::string coord_endpoint = coord_env;
			const int owner_skirmish_ai_id = CoordinatorOwnerSkirmishAiId();
			if (ai->GetSkirmishAIId() == owner_skirmish_ai_id) {
				coordinator_endpoint_ = coord_endpoint;
				coordinator_plugin_id_ = std::string("highbar-") + short_sha;
				coordinator_engine_sha256_ = grpc::kEngineSha256;
				grpc::LogError(
					ai,
					"GrpcGatewayModule",
					"deferring coordinator client-mode until first live frame for owner skirmishAIId="
					+ std::to_string(owner_skirmish_ai_id));
			} else {
				grpc::LogError(
					ai,
					"GrpcGatewayModule",
					"skipping coordinator client-mode for non-owner skirmishAIId="
					+ std::to_string(ai->GetSkirmishAIId())
					+ " owner=" + std::to_string(owner_skirmish_ai_id));
			}
		}

		ai->GetScheduler()->RunJobEvery(
			CScheduler::GameJob(&CGrpcGatewayModule::OnFrameTick, this),
			/*frameInterval=*/1, /*frameOffset=*/0);

	} catch (const std::exception& e) {
		grpc::LogFatalAndFailClosed(ai, "CGrpcGatewayModule::ctor", e.what());
		throw;
	}
}

// T011 — thread-safe fault request. Any thread (gRPC worker, serializer,
// handler) can call this; the transition itself runs next tick on the
// engine thread.
void CGrpcGatewayModule::RequestDisable(const std::string& subsystem,
                                          const std::string& reason,
                                          const std::string& detail) {
	if (state_.load(std::memory_order_acquire) != GatewayState::Healthy) return;
	std::lock_guard<std::mutex> lock(pending_fault_mutex_);
	if (!pending_fault_.has_value()) {
		pending_fault_ = PendingFault{subsystem, reason, detail};
	}
}

void CGrpcGatewayModule::DrainPendingFault() {
	std::optional<PendingFault> fault;
	{
		std::lock_guard<std::mutex> lock(pending_fault_mutex_);
		fault.swap(pending_fault_);
	}
	if (fault) {
		TransitionToDisabled(fault->subsystem, fault->reason, fault->detail);
	}
}

void CGrpcGatewayModule::EnsureLocalServiceBound(const char* reason) {
	if (service_bound_ || !deferred_service_bind_endpoint_.has_value() || !service_) return;
	AppendCoordinatorTrace(
		std::string("local service bind reason=")
		+ (reason != nullptr ? reason : "unknown")
		+ " frame=" + std::to_string(circuit != nullptr ? circuit->GetLastFrame() : -1));
	service_->Bind(*deferred_service_bind_endpoint_, &bound_address_);
	service_bound_ = true;
}

void CGrpcGatewayModule::EnsureCoordinatorClientStarted(const char* reason) {
	if (coordinator_client_ || coordinator_endpoint_.empty()) return;
	AppendCoordinatorTrace(
		std::string("coordinator start reason=")
		+ (reason != nullptr ? reason : "unknown")
		+ " frame=" + std::to_string(circuit != nullptr ? circuit->GetLastFrame() : -1));
	coordinator_client_ = std::make_unique<grpc::CoordinatorClient>(
		circuit, coordinator_endpoint_, coordinator_plugin_id_,
		coordinator_engine_sha256_);
	if (!coordinator_command_channel_started_) {
		coordinator_client_->StartCommandChannel(command_queue_.get());
		coordinator_command_channel_started_ = true;
	}
}

void CGrpcGatewayModule::MaybeEmitInitialCoordinatorSnapshot(const char* reason) {
	if (coordinator_initial_snapshot_sent_ || coordinator_endpoint_.empty()) return;
	if (circuit == nullptr || circuit->GetLastFrame() < 0) return;
	if (circuit->GetTeamUnits().empty()) return;
	EnsureCoordinatorClientStarted(reason);
	if (!coordinator_client_) return;
	coordinator_initial_snapshot_sent_ = true;
	AppendCoordinatorTrace(
		std::string("initial snapshot reason=")
		+ (reason != nullptr ? reason : "unknown")
		+ " frame=" + std::to_string(circuit->GetLastFrame())
		+ " own_units=" + std::to_string(circuit->GetTeamUnits().size()));
	BroadcastSnapshot(/*effective_cadence_frames=*/1);
}

// T011 — ordered side effects per data-model.md §2 and
// contracts/gateway-fault.md. Engine-thread only. Idempotent.
void CGrpcGatewayModule::TransitionToDisabled(const std::string& subsystem,
                                                const std::string& reason,
                                                const std::string& detail) {
	auto expected = GatewayState::Healthy;
	if (!state_.compare_exchange_strong(expected, GatewayState::Disabling,
	                                     std::memory_order_acq_rel)) {
		return;  // already Disabling or Disabled
	}

	const std::uint32_t frame = FrameForWire(circuit);

	// (1) Structured fault-log line.
	grpc::LogFault(circuit, subsystem, reason, detail, frame);

	// (2) Close subscriber streams with UNAVAILABLE + trailers.
	if (service_) {
		try { service_->FaultCloseAllStreams(subsystem, reason); }
		catch (...) { /* swallow during teardown */ }
	}

	// (3) Unlink UDS socket (TCP: socket_path_ is empty — no-op).
	if (!socket_path_.empty()) {
		::unlink(socket_path_.c_str());
	}

	// (4) Remove AI token file.
	if (!token_file_path_.empty()) {
		::unlink(token_file_path_.c_str());
	}

	// (5) Write-temp-and-rename the disabled health file.
	grpc::WriteHealthFile(health_file_path_, /*healthy=*/false,
	                       subsystem, reason, detail, frame);

	// (6) Release-store the terminal state.
	state_.store(GatewayState::Disabled, std::memory_order_release);
}

CGrpcGatewayModule::~CGrpcGatewayModule() {
	try {
		if (service_) service_->Shutdown();
		if (counters_ && circuit != nullptr) {
			grpc::LogShutdown(circuit, counters_->frames_since_bind.load());
		}
		if (token_) token_->Unlink();
	} catch (const std::exception& e) {
		if (circuit != nullptr) {
			grpc::LogError(circuit, "CGrpcGatewayModule::dtor", e.what());
		}
	} catch (...) {
		if (circuit != nullptr) {
			grpc::LogError(circuit, "CGrpcGatewayModule::dtor", "unknown exception");
		}
	}
}

// ============================================================================
// IModule event hooks (T037, T012)
// ============================================================================
//
// All hooks run on the engine thread. They append typed DeltaEvents
// to current_frame_delta_ without locking — current_frame_delta_ is
// engine-thread-owned. The shared/exclusive lock is taken in
// OnFrameTick, not here.
//
// T012 — every hook is wrapped in HB_HOOK_GUARD. On a caught exception
// the gateway transitions to Disabled on this same tick and every
// subsequent hook becomes a no-op. Early-return on Disabled is cheap
// (one acquire-load).

namespace {

void SetVec3(::highbar::v1::Vector3* dst, const springai::AIFloat3& src) {
	dst->set_x(src.x);
	dst->set_y(src.y);
	dst->set_z(src.z);
}

}  // namespace

// Void-returning hook guard. Early-returns on Disabled; catches every
// exception and routes it through TransitionToDisabled with subsystem
// "callback" (engine-callback threw into the gateway).
#define HB_HOOK_GUARD_VOID(body) \
	do { \
		if (state_.load(std::memory_order_acquire) != GatewayState::Healthy) return; \
		try { body } catch (...) { \
			TransitionToDisabled("callback", \
				grpc::ReasonCodeFor(std::current_exception()), \
				"engine_callback_threw"); \
			return; \
		} \
	} while (0)

// Int-returning hook guard — same, but returns 0 per IModule contract.
#define HB_HOOK_GUARD_INT(body) \
	do { \
		if (state_.load(std::memory_order_acquire) != GatewayState::Healthy) return 0; \
		try { body } catch (...) { \
			TransitionToDisabled("callback", \
				grpc::ReasonCodeFor(std::current_exception()), \
				"engine_callback_threw"); \
			return 0; \
		} \
	} while (0)

int CGrpcGatewayModule::UnitCreated(CCircuitUnit* unit, CCircuitUnit* builder) {
	HB_HOOK_GUARD_INT({
		if (unit == nullptr) return 0;
		auto* ev = current_frame_delta_.add_events()->mutable_unit_created();
		ev->set_unit_id(static_cast<std::int32_t>(unit->GetId()));
		ev->set_builder_id(
			builder != nullptr ? static_cast<std::int32_t>(builder->GetId()) : 0);
		MaybeEmitInitialCoordinatorSnapshot("unit_created");
		return 0;
	});
}

int CGrpcGatewayModule::UnitFinished(CCircuitUnit* unit) {
	HB_HOOK_GUARD_INT({
		if (unit == nullptr) return 0;
		auto* ev = current_frame_delta_.add_events()->mutable_unit_finished();
		ev->set_unit_id(static_cast<std::int32_t>(unit->GetId()));
		MaybeEmitInitialCoordinatorSnapshot("unit_finished");
		return 0;
	});
}

int CGrpcGatewayModule::UnitIdle(CCircuitUnit* unit) {
	HB_HOOK_GUARD_INT({
		if (unit == nullptr) return 0;
		order_state_tracker_->MarkIdle(
			static_cast<std::uint32_t>(unit->GetId()), CurrentFrame());
		auto* ev = current_frame_delta_.add_events()->mutable_unit_idle();
		ev->set_unit_id(static_cast<std::int32_t>(unit->GetId()));
		return 0;
	});
}

int CGrpcGatewayModule::UnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) {
	// T060 — no-op. The richer OnUnitDamagedFull entry (called from the
	// one surgical edit in CCircuitAI::HandleEvent) owns the UnitDamaged
	// delta; IModule's attacker-only signature is retained only for
	// IModule-contract compliance.
	(void)unit; (void)attacker;
	return 0;
}

void CGrpcGatewayModule::OnUnitDamagedFull(CCircuitUnit* unit,
                                            CEnemyInfo* attacker,
                                            float damage,
                                            const springai::AIFloat3& dir,
                                            int weaponDefId,
                                            bool paralyzer) {
	HB_HOOK_GUARD_VOID({
		if (unit == nullptr) return;
		auto* ev = current_frame_delta_.add_events()->mutable_unit_damaged();
		ev->set_unit_id(static_cast<std::int32_t>(unit->GetId()));
		if (attacker != nullptr) {
			ev->set_attacker_id(static_cast<std::int32_t>(attacker->GetId()));
		}
		// data-model.md §1: clamp negative damage (engine bug) rather
		// than propagate. weaponDefId verbatim (clients decide on -1).
		ev->set_damage(damage > 0.0f ? damage : 0.0f);
		SetVec3(ev->mutable_direction(), dir);
		ev->set_weapon_def_id(weaponDefId);
		ev->set_is_paralyzer(paralyzer);
	});
}

int CGrpcGatewayModule::UnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker) {
	HB_HOOK_GUARD_INT({
		if (unit == nullptr) return 0;
		order_state_tracker_->MarkUnitRemoved(
			static_cast<std::uint32_t>(unit->GetId()), CurrentFrame());
		auto* ev = current_frame_delta_.add_events()->mutable_unit_destroyed();
		ev->set_unit_id(static_cast<std::int32_t>(unit->GetId()));
		if (attacker != nullptr) {
			ev->set_attacker_id(static_cast<std::int32_t>(attacker->GetId()));
		}
		return 0;
	});
}

int CGrpcGatewayModule::UnitGiven(CCircuitUnit* unit, int oldTeam, int newTeam) {
	HB_HOOK_GUARD_INT({
		if (unit == nullptr) return 0;
		order_state_tracker_->MarkUnitRemoved(
			static_cast<std::uint32_t>(unit->GetId()), CurrentFrame());
		auto* ev = current_frame_delta_.add_events()->mutable_unit_given();
		ev->set_unit_id(static_cast<std::int32_t>(unit->GetId()));
		ev->set_old_team_id(oldTeam);
		ev->set_new_team_id(newTeam);
		return 0;
	});
}

int CGrpcGatewayModule::UnitCaptured(CCircuitUnit* unit, int oldTeam, int newTeam) {
	HB_HOOK_GUARD_INT({
		if (unit == nullptr) return 0;
		order_state_tracker_->MarkUnitRemoved(
			static_cast<std::uint32_t>(unit->GetId()), CurrentFrame());
		auto* ev = current_frame_delta_.add_events()->mutable_unit_captured();
		ev->set_unit_id(static_cast<std::int32_t>(unit->GetId()));
		ev->set_old_team_id(oldTeam);
		ev->set_new_team_id(newTeam);
		return 0;
	});
}

void CGrpcGatewayModule::OnUnitMoveFailed(CCircuitUnit* unit) {
	HB_HOOK_GUARD_VOID({
		if (unit == nullptr) return;
		auto* ev = current_frame_delta_.add_events()->mutable_unit_move_failed();
		ev->set_unit_id(static_cast<std::int32_t>(unit->GetId()));
	});
}

void CGrpcGatewayModule::OnEnemyEnterLOS(CEnemyInfo* enemy) {
	HB_HOOK_GUARD_VOID({
		if (enemy == nullptr) return;
		auto* ev = current_frame_delta_.add_events()->mutable_enemy_enter_los();
		ev->set_enemy_id(static_cast<std::int32_t>(enemy->GetId()));
	});
}

void CGrpcGatewayModule::OnEnemyLeaveLOS(CEnemyInfo* enemy) {
	HB_HOOK_GUARD_VOID({
		if (enemy == nullptr) return;
		auto* ev = current_frame_delta_.add_events()->mutable_enemy_leave_los();
		ev->set_enemy_id(static_cast<std::int32_t>(enemy->GetId()));
	});
}

void CGrpcGatewayModule::OnEnemyEnterRadar(CEnemyInfo* enemy) {
	HB_HOOK_GUARD_VOID({
		if (enemy == nullptr) return;
		auto* ev = current_frame_delta_.add_events()->mutable_enemy_enter_radar();
		ev->set_enemy_id(static_cast<std::int32_t>(enemy->GetId()));
	});
}

void CGrpcGatewayModule::OnEnemyLeaveRadar(CEnemyInfo* enemy) {
	HB_HOOK_GUARD_VOID({
		if (enemy == nullptr) return;
		auto* ev = current_frame_delta_.add_events()->mutable_enemy_leave_radar();
		ev->set_enemy_id(static_cast<std::int32_t>(enemy->GetId()));
	});
}

void CGrpcGatewayModule::OnEnemyDamaged(CEnemyInfo* enemy) {
	HB_HOOK_GUARD_VOID({
		if (enemy == nullptr) return;
		auto* ev = current_frame_delta_.add_events()->mutable_enemy_damaged();
		ev->set_enemy_id(static_cast<std::int32_t>(enemy->GetId()));
	});
}

void CGrpcGatewayModule::OnEnemyDestroyed(CEnemyInfo* enemy) {
	HB_HOOK_GUARD_VOID({
		if (enemy == nullptr) return;
		auto* ev = current_frame_delta_.add_events()->mutable_enemy_destroyed();
		ev->set_enemy_id(static_cast<std::int32_t>(enemy->GetId()));
	});
}

void CGrpcGatewayModule::OnFeatureCreated(int feature_id, int def_id,
                                           float px, float py, float pz) {
	HB_HOOK_GUARD_VOID({
		auto* ev = current_frame_delta_.add_events()->mutable_feature_created();
		ev->set_feature_id(static_cast<std::uint32_t>(feature_id));
		ev->set_def_id(static_cast<std::uint32_t>(def_id));
		ev->mutable_position()->set_x(px);
		ev->mutable_position()->set_y(py);
		ev->mutable_position()->set_z(pz);
	});
}

void CGrpcGatewayModule::OnFeatureDestroyed(int feature_id) {
	HB_HOOK_GUARD_VOID({
		auto* ev = current_frame_delta_.add_events()->mutable_feature_destroyed();
		ev->set_feature_id(static_cast<std::uint32_t>(feature_id));
	});
}

void CGrpcGatewayModule::OnEconomyTick() {
	HB_HOOK_GUARD_VOID({
		// Aggregate economy into a single EconomyTick event. Called from
		// OnFrameTick so the rate is bounded.
		auto* ev = current_frame_delta_.add_events()->mutable_economy_tick();
		auto* em = circuit->GetEconomyManager();
		if (em == nullptr) return;
		ev->set_metal(em->GetMetalCur());
		ev->set_metal_storage(em->GetMetalStore());
		ev->set_metal_usage(em->GetMetalPull());
		ev->set_energy(em->GetEnergyCur());
		ev->set_energy_storage(em->GetEnergyStore());
		ev->set_energy_usage(em->GetEnergyPull());
		// income fields: left 0 until verified accessor lands.
	});
}

// ============================================================================
// Frame tick (T026 + T038 + T039)
// ============================================================================

void CGrpcGatewayModule::OnFrameTick() {
	// T012/T014 — at the top of every tick, execute any worker-thread
	// fault request first, then short-circuit if already disabled.
	DrainPendingFault();
	if (state_.load(std::memory_order_acquire) != GatewayState::Healthy) return;

	try {
		if (service_) service_->AdvanceFrame();

		// Client-mode starts after pregame. Constructing gRPC client
		// channels during Spring's pregame network join path can prevent
		// graphical spectators from attaching; wait until the first live
		// frame so watched runs get their attach window first.
		++frame_counter_;
		const auto live_frame = circuit != nullptr ? circuit->GetLastFrame() : -1;
		if (live_frame >= 0) {
			EnsureLocalServiceBound("live_frame");
		}
		if (!coordinator_client_ && live_frame >= 0) {
			EnsureCoordinatorClientStarted("live_frame");
		}
		MaybeEmitInitialCoordinatorSnapshot("frame_tick");
		// Client-mode heartbeat. Engine-thread; CoordinatorClient's
		// SendHeartbeat has a 250ms deadline so stalls are bounded.
		if (coordinator_client_ && (frame_counter_ % kHeartbeatEveryNFrames) == 0) {
			coordinator_client_->SendHeartbeat(frame_counter_);
		}

		// Minimal InvokeCallback bridge: resolve queued synchronous
		// callback requests on the engine thread before any command
		// dispatch so callers waiting on def metadata wake promptly.
		DrainCallbackQueue();
		if (admin_service_ != nullptr) {
			admin_service_->ExpireLeases();
		}
		DrainAdminActionQueue();

		// T057: drain external-AI commands at the top of the frame so
		// they land in the engine this tick. Engine-thread only.
		DrainCommandQueue();

		// 003-snapshot-arm-coverage T012 — pump the snapshot scheduler.
		// Engine-thread only; inherits the same frame-scope as the
		// delta flush below. The tick call is cheap when not firing
		// (a single branch on next_snapshot_frame_), so unconditional
		// invocation is fine.
		{
			const std::uint32_t frame = FrameForWire(circuit);
			current_frame_.store(frame, std::memory_order_release);
			const std::size_t own_units_count = circuit != nullptr
				? circuit->GetTeamUnits().size() : 0;
			const auto pump = snapshot_tick_.Pump(frame, own_units_count);
			if (pump.emit) {
				BroadcastSnapshot(pump.effective_cadence_frames);
			}
		}

		// Emit an EconomyTick every 30 frames (1s at 30Hz). Keeps the
		// observers' economy plot moving without flooding the delta stream.
		static constexpr std::uint32_t kEconomyEveryNFrames = 30;
		const auto frames = counters_ != nullptr
			? counters_->frames_since_bind.load() : 0u;
		if (frames > 0 && frames % kEconomyEveryNFrames == 0) {
			OnEconomyTick();
		}

		// T038: flush accumulated delta. Take the exclusive lock only for
		// the serialize-and-publish step; the append path above is
		// engine-thread-owned and holds nothing.
		if (current_frame_delta_.events_size() > 0) {
			FlushDelta();
			frames_since_last_flush_ = 0;
		} else {
			++frames_since_last_flush_;
			if (frames_since_last_flush_ >= kKeepAliveFrames) {
				EmitKeepAlive();
				frames_since_last_flush_ = 0;
			}
		}
	} catch (...) {
		TransitionToDisabled("callback",
			grpc::ReasonCodeFor(std::current_exception()),
			"frame_tick_threw");
	}
}

void CGrpcGatewayModule::FlushDelta() {
	// T014 — guard the serializer hot path. OOM or protobuf failure here
	// transitions to Disabled with subsystem=serialization.
	try {
		const std::uint64_t t0 = NowMicros();

		::highbar::v1::StateUpdate update;
		update.set_seq(++seq_);
		update.set_frame(FrameForWire(circuit));
		*update.mutable_delta() = std::move(current_frame_delta_);
		current_frame_delta_.Clear();

		auto payload = std::make_shared<std::string>();
		if (!update.SerializeToString(payload.get())) {
			throw std::runtime_error("SerializeToString failed");
		}
		auto frozen = std::const_pointer_cast<const std::string>(payload);

		{
			std::unique_lock<std::shared_mutex> lock(state_mutex_);
			ring_->Push(seq_, frozen);
		}
		delta_bus_->Publish(frozen);

		// Phase B — client-mode push. The coordinator receives the
		// same StateUpdate that the in-process ring stores, so any
		// external observer attached to the coordinator sees a feed
		// equivalent to what server-mode StreamState would have
		// produced. Serialization is already done; we pass the object
		// itself (gRPC does its own copy for wire-level framing).
		if (coordinator_client_) {
			coordinator_client_->PushStateUpdate(update);
		}

		if (counters_ != nullptr) {
			counters_->RecordFrameFlushUs(NowMicros() - t0);
		}
	} catch (...) {
		TransitionToDisabled("serialization",
			grpc::ReasonCodeFor(std::current_exception()),
			"flush_delta_threw");
	}
}

void CGrpcGatewayModule::EmitKeepAlive() {
	try {
		::highbar::v1::StateUpdate update;
		update.set_seq(++seq_);
		update.set_frame(FrameForWire(circuit));
		update.mutable_keepalive();

		auto payload = std::make_shared<std::string>();
		if (!update.SerializeToString(payload.get())) {
			throw std::runtime_error("SerializeToString failed");
		}
		auto frozen = std::const_pointer_cast<const std::string>(payload);

		{
			std::unique_lock<std::shared_mutex> lock(state_mutex_);
			ring_->Push(seq_, frozen);
		}
		delta_bus_->Publish(frozen);

		// Phase B — also push keepalives to the coordinator so an
		// idle observer-side client can distinguish "connected but no
		// state events" from "connection hung".
		if (coordinator_client_) {
			coordinator_client_->PushStateUpdate(update);
		}
	} catch (...) {
		TransitionToDisabled("serialization",
			grpc::ReasonCodeFor(std::current_exception()),
			"keepalive_threw");
	}
}

// 003-snapshot-arm-coverage T011 — build a StateSnapshot via the existing
// SnapshotBuilder, wrap in a StateUpdate, and push through ring + DeltaBus
// + optional coordinator. Stamps effective_cadence_frames + send_monotonic_ns
// per contracts/snapshot-tick.md §Scheduler behavior invariants 2–7.
//
// Engine-thread only. Takes state_mutex_ exclusive for the ring push, same
// as FlushDelta. On serializer/OOM failure transitions to Disabled with
// subsystem=serialization, same failure mode as FlushDelta.
void CGrpcGatewayModule::BroadcastSnapshot(std::uint32_t effective_cadence_frames) {
	if (snapshot_ == nullptr || ring_ == nullptr || delta_bus_ == nullptr) return;
	try {
		const std::uint64_t t0 = NowMicros();

		::highbar::v1::StateUpdate update;
		update.set_seq(++seq_);
		update.set_frame(FrameForWire(circuit));

		// Build the snapshot. BuildIncremental omits StaticMap — it was
		// already delivered in HelloResponse and on any StreamState
		// resume-from-empty path; per-tick resends would be wasted bytes.
		auto* snap = update.mutable_snapshot();
		*snap = snapshot_->BuildIncremental();
		snap->set_effective_cadence_frames(effective_cadence_frames);
		snap->set_frame_number(FrameForWire(circuit));

		// Constitution V: stamp CLOCK_MONOTONIC_ns at the moment we hand
		// the frame to the fan-out. Same pattern as CoordinatorClient.
		{
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			update.set_send_monotonic_ns(
				static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL
				+ static_cast<std::uint64_t>(ts.tv_nsec));
		}

		auto payload = std::make_shared<std::string>();
		if (!update.SerializeToString(payload.get())) {
			throw std::runtime_error("SerializeToString failed");
		}
		auto frozen = std::const_pointer_cast<const std::string>(payload);

		{
			std::unique_lock<std::shared_mutex> lock(state_mutex_);
			ring_->Push(seq_, frozen);
		}
		delta_bus_->Publish(frozen);

		if (coordinator_client_) {
			coordinator_client_->PushStateUpdate(update);
		}

		if (counters_ != nullptr) {
			counters_->RecordFrameFlushUs(NowMicros() - t0);
		}
	} catch (...) {
		TransitionToDisabled("serialization",
			grpc::ReasonCodeFor(std::current_exception()),
			"broadcast_snapshot_threw");
	}
}

std::uint64_t CGrpcGatewayModule::HeadSeq() const {
	return ring_ ? ring_->HeadSeq() : 0;
}

::highbar::v1::AdminActionResult CGrpcGatewayModule::QueueAdminAction(
		const ::circuit::grpc::AdminCaller& caller,
		const ::highbar::v1::AdminAction& action,
		std::chrono::milliseconds timeout) {
	if (state_.load(std::memory_order_acquire) != GatewayState::Healthy
	    || admin_controller_ == nullptr) {
		::highbar::v1::AdminActionResult result;
		result.set_action_seq(action.action_seq());
		result.set_client_action_id(action.client_action_id());
		result.set_status(::highbar::v1::ADMIN_ACTION_NOT_DISPATCHED);
		result.set_frame(CurrentFrame());
		result.set_state_seq(HeadSeq());
		result.set_dry_run(false);
		auto* issue = result.add_issues();
		issue->set_code(::highbar::v1::ADMIN_ENGINE_NOT_READY);
		issue->set_field_path("action");
		issue->set_detail("gateway engine thread is unavailable");
		issue->set_retry_hint(::highbar::v1::RETRY_AFTER_NEXT_SNAPSHOT);
		return result;
	}

	auto pending = std::make_shared<PendingAdminAction>();
	pending->caller = caller;
	pending->action = action;
	auto future = pending->completion.get_future();
	{
		std::lock_guard<std::mutex> lock(pending_admin_actions_mutex_);
		pending_admin_actions_.push_back(pending);
	}

	if (future.wait_for(timeout) != std::future_status::ready) {
		::highbar::v1::AdminActionResult result;
		result.set_action_seq(action.action_seq());
		result.set_client_action_id(action.client_action_id());
		result.set_status(::highbar::v1::ADMIN_ACTION_NOT_DISPATCHED);
		result.set_frame(CurrentFrame());
		result.set_state_seq(HeadSeq());
		result.set_dry_run(false);
		auto* issue = result.add_issues();
		issue->set_code(::highbar::v1::ADMIN_ISSUE_ACTION_NOT_DISPATCHED);
		issue->set_field_path("action");
		issue->set_detail("timed out waiting for engine-thread admin execution");
		issue->set_retry_hint(::highbar::v1::RETRY_AFTER_NEXT_SNAPSHOT);
		return result;
	}
	return future.get();
}

CGrpcGatewayModule::CallbackRpcStatus CGrpcGatewayModule::InvokeCallback(
	const ::highbar::v1::CallbackRequest& request,
	::highbar::v1::CallbackResponse* response,
	std::chrono::milliseconds timeout,
	std::string* error_detail) {
	if (response == nullptr) {
		if (error_detail != nullptr) {
			*error_detail = "null InvokeCallback response sink";
		}
		return CallbackRpcStatus::Internal;
	}
	if (state_.load(std::memory_order_acquire) != GatewayState::Healthy) {
		if (error_detail != nullptr) {
			*error_detail = "gateway is not healthy";
		}
		return CallbackRpcStatus::FailedPrecondition;
	}

	auto pending = std::make_shared<PendingCallbackInvocation>();
	pending->request = request;
	auto future = pending->completion.get_future();
	{
		std::lock_guard<std::mutex> lock(pending_callbacks_mutex_);
		pending_callbacks_.push_back(pending);
	}

	if (future.wait_for(timeout) != std::future_status::ready) {
		if (error_detail != nullptr) {
			*error_detail = "InvokeCallback timed out waiting for engine-thread response";
		}
		return CallbackRpcStatus::Internal;
	}

	auto result = future.get();
	if (error_detail != nullptr) {
		*error_detail = result.error_detail;
	}
	if (result.status == CallbackRpcStatus::Ok) {
		*response = std::move(result.response);
	}
	return result.status;
}

// ============================================================================
// Command drain (T057)
// ============================================================================
//
// Called from OnFrameTick at the top of every frame. Pulls queued
// commands out of the MPSC CommandQueue and dispatches each to the
// matching CCircuitUnit::Cmd*. Re-resolves the target unit here
// rather than trusting the worker-thread validation result — a unit
// can die between validate-at-submission and drain-at-frame.
void CGrpcGatewayModule::DrainCallbackQueue() {
	if (circuit == nullptr) return;

	std::deque<std::shared_ptr<PendingCallbackInvocation>> pending;
	{
		std::lock_guard<std::mutex> lock(pending_callbacks_mutex_);
		if (pending_callbacks_.empty()) return;
		pending.swap(pending_callbacks_);
	}

	for (auto& entry : pending) {
		CallbackInvocationResult result;
		try {
			switch (entry->request.callback_id()) {
			case ::highbar::v1::CALLBACK_GET_UNIT_DEFS: {
				if (entry->request.params_size() != 0) {
					result = MakeCallbackFailure(entry->request,
					                             "CALLBACK_GET_UNIT_DEFS does not take params");
					break;
				}
				result = MakeCallbackSuccess(entry->request);
				auto* values = result.response.mutable_result()->mutable_int_array_value();
				for (const auto& cdef : circuit->GetCircuitDefs()) {
					values->add_values(static_cast<std::int32_t>(cdef.GetId()));
				}
				break;
			}
			case ::highbar::v1::CALLBACK_UNITDEF_GET_NAME: {
				std::string param_error;
				const auto def_id = ExtractSingleIntParam(entry->request, &param_error);
				if (!def_id.has_value()) {
					result = MakeCallbackFailure(entry->request, std::move(param_error));
					break;
				}
				auto* cdef = circuit->GetCircuitDefSafe(
					static_cast<CCircuitDef::Id>(*def_id));
				if (cdef == nullptr || cdef->GetDef() == nullptr) {
					result = MakeCallbackFailure(
						entry->request,
						"unknown unit_def_id=" + std::to_string(*def_id));
					break;
				}
				result = MakeCallbackSuccess(entry->request);
				result.response.mutable_result()->set_string_value(
					cdef->GetDef()->GetName());
				break;
			}
			default:
				result = MakeCallbackFailure(
					entry->request,
					"unsupported callback_id=" + std::to_string(entry->request.callback_id()));
				break;
			}
		} catch (const std::exception& e) {
			result = MakeCallbackRpcError(entry->request,
			                              CallbackRpcStatus::Internal,
			                              e.what());
		} catch (...) {
			result = MakeCallbackRpcError(entry->request,
			                              CallbackRpcStatus::Internal,
			                              "unknown exception in DrainCallbackQueue");
		}
		entry->completion.set_value(std::move(result));
	}
}

void CGrpcGatewayModule::DrainAdminActionQueue() {
	if (admin_controller_ == nullptr) return;

	std::deque<std::shared_ptr<PendingAdminAction>> pending;
	{
		std::lock_guard<std::mutex> lock(pending_admin_actions_mutex_);
		if (pending_admin_actions_.empty()) return;
		pending.swap(pending_admin_actions_);
	}

	for (auto& entry : pending) {
		auto result = admin_controller_->Validate(
			entry->caller, entry->action, CurrentFrame(), HeadSeq());
		result.set_dry_run(false);
		if (result.status() == ::highbar::v1::ADMIN_ACTION_ACCEPTED) {
			if (ApplyAdminAction(entry->action)) {
				result = admin_controller_->Execute(
					entry->caller, entry->action, CurrentFrame(), HeadSeq());
			} else {
				result.set_status(::highbar::v1::ADMIN_ACTION_NOT_DISPATCHED);
				auto* issue = result.add_issues();
				issue->set_code(::highbar::v1::ADMIN_ISSUE_ACTION_NOT_DISPATCHED);
				issue->set_field_path("action");
				issue->set_detail("engine rejected admin action dispatch");
				issue->set_retry_hint(::highbar::v1::RETRY_AFTER_NEXT_SNAPSHOT);
			}
		}
		entry->completion.set_value(std::move(result));
	}
}

bool CGrpcGatewayModule::ApplyAdminAction(
		const ::highbar::v1::AdminAction& action) {
	if (circuit == nullptr) return false;

	using A = ::highbar::v1::AdminAction;
	switch (action.action_case()) {
	case A::kPause: {
		auto* game = circuit->GetGame();
		if (game == nullptr) return false;
		game->SetPause(action.pause().paused(), "via highbar admin");
		return true;
	}
	case A::kGlobalSpeed:
		if (SendAutohostCommandDatagrams(
			    GlobalSpeedAutohostCommands(action.global_speed().speed()))) {
			return true;
		}
		if (auto* lua = circuit->GetLua(); lua != nullptr) {
			const std::string message =
				GlobalSpeedLuaMessage(action.global_speed().speed());
			const int message_size = static_cast<int>(message.size());
			if (lua->CallUI(message.c_str(), message_size) == "ok") {
				return true;
			}
			return lua->CallRules(message.c_str(), message_size) == "ok";
		}
		return false;
	case A::kCheatPolicy: {
		auto* cheats = circuit->GetCheats();
		return cheats != nullptr && cheats->SetEnabled(action.cheat_policy().enabled());
	}
	case A::kResourceGrant: {
		auto* callback = circuit->GetCallback();
		if (callback == nullptr) return false;
		const char* name = action.resource_grant().resource_id() == 0
			? "Metal" : "Energy";
		auto* resource = callback->GetResourceByName(name);
		if (resource == nullptr) return false;
		if (action.resource_grant().team_id() == circuit->GetTeamId()) {
			auto* cheats = circuit->GetCheats();
			if (cheats == nullptr) {
				delete resource;
				return false;
			}
			bool restore_disabled = false;
			try {
				const bool was_enabled = cheats->IsEnabled();
				if (!was_enabled) {
					if (!cheats->SetEnabled(true)) {
						delete resource;
						return false;
					}
					restore_disabled = true;
				}
				cheats->GiveMeResource(resource, action.resource_grant().amount());
				if (restore_disabled) (void)cheats->SetEnabled(false);
			} catch (...) {
				if (restore_disabled) {
					try { (void)cheats->SetEnabled(false); } catch (...) {}
				}
				delete resource;
				return false;
			}
		} else {
			auto* economy = callback->GetEconomy();
			if (economy == nullptr) {
				delete resource;
				return false;
			}
			economy->SendResource(
				resource,
				action.resource_grant().amount(),
				action.resource_grant().team_id());
		}
		delete resource;
		return true;
	}
	case A::kUnitSpawn: {
		auto* cheats = circuit->GetCheats();
		if (cheats == nullptr) return false;
		auto* def = circuit->GetCircuitDefSafe(
			static_cast<CCircuitDef::Id>(action.unit_spawn().unit_def_id()));
		if (def == nullptr || def->GetDef() == nullptr) return false;
		bool restore_disabled = false;
		try {
			const bool was_enabled = cheats->IsEnabled();
			if (!was_enabled) {
				if (!cheats->SetEnabled(true)) return false;
				restore_disabled = true;
			}
			const int unit_id = cheats->GiveMeUnit(
				def->GetDef(), AdminToFloat3(action.unit_spawn().position()));
			if (restore_disabled) (void)cheats->SetEnabled(false);
			if (unit_id <= 0) return false;
			if (action.unit_spawn().team_id() != circuit->GetTeamId()) {
				auto* spawned = circuit->GetOrRegTeamUnit(
					static_cast<ICoreUnit::Id>(unit_id));
				if (spawned == nullptr) return false;
				std::vector<CCircuitUnit*> units{spawned};
				circuit->GiveUnits(std::move(units), action.unit_spawn().team_id());
			}
			return true;
		} catch (...) {
			if (restore_disabled) {
				try { (void)cheats->SetEnabled(false); } catch (...) {}
			}
			return false;
		}
	}
	case A::kUnitTransfer: {
		auto* unit = circuit->GetTeamUnit(
			static_cast<ICoreUnit::Id>(action.unit_transfer().unit_id()));
		if (unit == nullptr || unit->IsDead()) return false;
		std::vector<CCircuitUnit*> units{unit};
		circuit->GiveUnits(std::move(units), action.unit_transfer().to_team_id());
		return true;
	}
	case A::kLifecycle:
		return true;
	default:
		return false;
	}
}

void CGrpcGatewayModule::DrainCommandQueue() {
	if (command_queue_ == nullptr || circuit == nullptr) return;

	std::vector<grpc::QueuedCommand> batch;
	const std::size_t drained = command_queue_->Drain(&batch);
	if (drained == 0) return;

	for (auto& entry : batch) {
		// The validator accepted a single authoritative batch target and
		// that normalized target is preserved on the queue entry.
		const auto& cmd = entry.command;
		auto* dispatch_event =
			current_frame_delta_.add_events()->mutable_command_dispatch();
		dispatch_event->set_batch_seq(entry.batch_seq);
		dispatch_event->set_client_command_id(entry.client_command_id);
		dispatch_event->set_command_index(entry.command_index);
		dispatch_event->set_target_unit_id(
			static_cast<std::uint32_t>(entry.authoritative_target_unit_id));
		dispatch_event->set_frame(CurrentFrame());
		dispatch_event->set_channel_incarnation(entry.channel_incarnation);
		const auto target_id = grpc::EffectiveDispatchTargetUnitId(
			entry.authoritative_target_unit_id, cmd);
		if (!target_id.has_value()) {
			dispatch_event->set_status(
				::highbar::v1::COMMAND_DISPATCH_SKIPPED_UNSUPPORTED_ARM);
			auto* issue = dispatch_event->mutable_issue();
			issue->set_code(::highbar::v1::COMMAND_ARM_NOT_DISPATCHED);
			issue->set_field_path("commands");
			issue->set_detail("command arm is not dispatched");
			issue->set_retry_hint(::highbar::v1::RETRY_WITH_FRESH_CAPABILITIES);
			continue;
		}

		// target_id == -1 marks game-wide arms (Game / Pathing / Lua /
		// Cheats) that don't bind to any unit. Use any own unit as the
		// dispatch context; the dispatcher case body ignores `unit` for
		// these arms but DispatchCommand's guards still want non-null.
		CCircuitUnit* unit_ctx = nullptr;
		if (*target_id > 0) {
			unit_ctx = circuit->GetTeamUnit(*target_id);
			if (unit_ctx == nullptr) {
				dispatch_event->set_status(
					::highbar::v1::COMMAND_DISPATCH_SKIPPED_TARGET_MISSING);
				auto* issue = dispatch_event->mutable_issue();
				issue->set_code(::highbar::v1::MISSING_TARGET_UNIT);
				issue->set_field_path("target_unit_id");
				issue->set_detail("target unit missing at dispatch time");
				issue->set_retry_hint(::highbar::v1::RETRY_AFTER_NEXT_SNAPSHOT);
				continue;
			}
			if (unit_ctx->IsDead()) {
				dispatch_event->set_status(
					::highbar::v1::COMMAND_DISPATCH_SKIPPED_TARGET_DEAD);
				auto* issue = dispatch_event->mutable_issue();
				issue->set_code(::highbar::v1::TARGET_UNIT_DEAD);
				issue->set_field_path("target_unit_id");
				issue->set_detail("target unit dead at dispatch time");
				issue->set_retry_hint(::highbar::v1::RETRY_AFTER_NEXT_SNAPSHOT);
				continue;
			}
		} else {
			const auto& own = circuit->GetTeamUnits();
			if (own.empty()) {
				dispatch_event->set_status(
					::highbar::v1::COMMAND_DISPATCH_SKIPPED_ENGINE_NOT_READY);
				continue;
			}
			unit_ctx = own.begin()->second;  // any live unit
			if (unit_ctx == nullptr) {
				dispatch_event->set_status(
					::highbar::v1::COMMAND_DISPATCH_SKIPPED_ENGINE_NOT_READY);
				continue;
			}
		}

		// T015 — dispatch hot path guard. Engine-thread only; direct
		// TransitionToDisabled is safe.
		try {
			AppendCoordinatorTrace(
				"dispatch begin kind=" + std::string(CommandKind(cmd))
				+ " target=" + std::to_string(*target_id));
			if (grpc::DispatchCommand(circuit, unit_ctx, cmd)) {
				dispatch_event->set_status(
					::highbar::v1::COMMAND_DISPATCH_APPLIED);
				if (*target_id > 0) {
					order_state_tracker_->MarkAccepted(
						static_cast<std::uint32_t>(*target_id),
						entry.batch_seq,
						entry.client_command_id,
						CurrentFrame(),
						CommandKind(cmd));
				}
			} else {
				dispatch_event->set_status(
					::highbar::v1::COMMAND_DISPATCH_SKIPPED_UNSUPPORTED_ARM);
				auto* issue = dispatch_event->mutable_issue();
				issue->set_code(::highbar::v1::COMMAND_ARM_NOT_DISPATCHED);
				issue->set_field_path("commands");
				issue->set_detail("command arm was skipped by dispatcher");
				issue->set_retry_hint(::highbar::v1::RETRY_WITH_FRESH_CAPABILITIES);
			}
			AppendCoordinatorTrace(
				"dispatch end kind=" + std::string(CommandKind(cmd))
				+ " target=" + std::to_string(*target_id));
		} catch (...) {
			const std::string reason = grpc::ReasonCodeFor(std::current_exception());
			std::string detail =
				"dispatch_threw kind=" + std::string(CommandKind(cmd))
				+ " target=" + std::to_string(*target_id);
			try {
				std::rethrow_exception(std::current_exception());
			} catch (const std::exception& e) {
				detail += " what=" + std::string(e.what());
			} catch (...) {
				detail += " what=unknown";
			}
			TransitionToDisabled("dispatch",
				reason,
				detail);
			return;
		}
	}
}

}  // namespace circuit
