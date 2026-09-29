// SPDX-License-Identifier: GPL-2.0-only
//
// HighBarV3 — IModule wrapper that owns the gRPC gateway (T016, plus
// US1 extensions T036/T037/T038/T039/T042).
//
// Construction loads data/config/grpc.json, generates and writes the
// AI auth token, binds the gRPC server, and wires:
//   * SnapshotBuilder     — materializes StateSnapshot on subscribe.
//   * DeltaBus + SubscriberSlot — per-subscriber fan-out rings.
//   * RingBuffer          — 2048-entry resume-history buffer.
//   * state_mutex_        — shared/exclusive lock separating engine
//                           writers from worker snapshot reads (T036).
//   * current_frame_delta_ — per-frame StateDelta accumulator.
//
// IModule event handlers (UnitCreated, UnitDamaged, …) and the
// non-virtual On*Event methods append typed DeltaEvents to
// current_frame_delta_ on the engine thread (T037). OnFrameTick
// serializes the delta, pushes to RingBuffer, publishes through
// DeltaBus, clears the accumulator (T038). If no delta flushed for
// `kKeepAliveFrames` frames the tick emits a KeepAlive instead
// (T039, data-model §2).

#pragma once

#include "module/Module.h"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>

#include "highbar/callbacks.pb.h"
#include "highbar/service.pb.h"
#include "highbar/state.pb.h"
#include "highbar/live_control.pb.h"
#include "grpc/Config.h"
#include "grpc/AdminController.h"
#include "grpc/SnapshotTick.h"
#include "grpc/StateUpdateProjection.h"

namespace circuit::grpc {
class HighBarService;
class Counters;
class AuthToken;
class AdminService;
class SnapshotBuilder;
class DeltaBus;
class RingBuffer;
class CommandQueue;
class OrderStateTracker;
class CoordinatorClient;
class LiveControlState;
class FeatureLifetimeLedger;
class MixedLifecycleQualification;
struct QueuedCommand;
}  // namespace circuit::grpc

namespace circuit {

// T009 — gateway runtime health state (data-model.md §2).
// Monotonic: Healthy → Disabling → Disabled; no return to Healthy in
// the same match.
enum class GatewayState : std::uint8_t {
	Healthy = 0,
	Disabling = 1,
	Disabled = 2,
};

class CGrpcGatewayModule final : public IModule {
public:
	explicit CGrpcGatewayModule(CCircuitAI* circuit);
	~CGrpcGatewayModule() override;

	CGrpcGatewayModule(const CGrpcGatewayModule&) = delete;
	CGrpcGatewayModule& operator=(const CGrpcGatewayModule&) = delete;

	// V3 — the gateway has no AngelScript, so InitScript must bypass
	// the base's `script->Init()` call (script is nullptr for us).
	// Without this, CCircuitAI::Init dereferences null and the match
	// never advances past frame -1.
	bool InitScript() override { return true; }

	// IModule event hooks (T037 — append to current_frame_delta_).
	int UnitCreated(CCircuitUnit* unit, CCircuitUnit* builder) override;
	int UnitFinished(CCircuitUnit* unit) override;
	int UnitIdle(CCircuitUnit* unit) override;
	int UnitDamaged(CCircuitUnit* unit, CEnemyInfo* attacker) override;
	int UnitDestroyed(CCircuitUnit* unit, CEnemyInfo* attacker) override;
	int UnitGiven(CCircuitUnit* unit, int oldTeamId, int newTeamId) override;
	int UnitCaptured(CCircuitUnit* unit, int oldTeamId, int newTeamId) override;

	// T058 — rich UnitDamaged event. IModule::UnitDamaged(unit, attacker)
	// drops damage / dir / weaponDefId / paralyzer on the floor; this
	// variant receives them directly from CCircuitAI::HandleEvent's
	// EVENT_UNIT_DAMAGED dispatch (the one surgical edit in
	// CircuitAI.cpp keeps 001's Constitution I envelope intact).
	void OnUnitDamagedFull(CCircuitUnit* unit,
	                       CEnemyInfo* attacker,
	                       float damage,
	                       const springai::AIFloat3& dir,
	                       int weaponDefId,
	                       bool paralyzer);

	// Non-virtual on CircuitAI side (invoked from CircuitAI.cpp's
	// event dispatch — wiring of those call sites is a follow-up
	// upstream-shared edit; at Phase 2 the gateway simply provides
	// the hook points).
	void OnUnitMoveFailed(CCircuitUnit* unit);
	void OnEnemyEnterLOS(CEnemyInfo* enemy);
	void OnEnemyLeaveLOS(CEnemyInfo* enemy);
	void OnEnemyEnterRadar(CEnemyInfo* enemy);
	void OnEnemyLeaveRadar(CEnemyInfo* enemy);
	void OnEnemyDamaged(CEnemyInfo* enemy);
	void OnEnemyDestroyed(CEnemyInfo* enemy);
	void OnFeatureCreated(int feature_id, int def_id,
	                      float px, float py, float pz);
	void OnFeatureDestroyed(int feature_id);
	void OnEconomyTick();  // called from OnFrameTick every kEconomyEveryNFrames

	// Frame tick — registered in ctor via CScheduler::RunJobEvery. On
	// every frame: drains (future) CommandQueue, serializes delta,
	// publishes, emits KeepAlive on quiet, advances counters.
	void OnFrameTick();

	// Accessors for HighBarService (Hello's StaticMap + StreamState's
	// subscribe). Non-null for the lifetime of the module.
	::circuit::grpc::SnapshotBuilder* GetSnapshotBuilder() { return snapshot_.get(); }
	::circuit::grpc::DeltaBus*        GetDeltaBus()        { return delta_bus_.get(); }
	::circuit::grpc::RingBuffer*      GetRingBuffer()      { return ring_.get(); }
	::circuit::grpc::CommandQueue*    GetCommandQueue()    { return command_queue_.get(); }
	std::shared_mutex&                StateMutex()         { return state_mutex_; }
	std::uint64_t                     HeadSeq() const;

	enum class CallbackRpcStatus : std::uint8_t {
		Ok = 0,
		Unavailable = 1,
		FailedPrecondition = 2,
		Internal = 3,
	};

	struct CallbackInvocationResult {
		CallbackRpcStatus status = CallbackRpcStatus::Internal;
		std::string error_detail;
		::highbar::v1::CallbackResponse response;
	};

	// Worker-thread entry point for InvokeCallback. The request is queued
	// for engine-thread execution and this call blocks up to `timeout`
	// waiting for the result. The returned response is only meaningful
	// when the status is Ok.
	CallbackRpcStatus InvokeCallback(
		const ::highbar::v1::CallbackRequest& request,
		::highbar::v1::CallbackResponse* response,
		std::chrono::milliseconds timeout,
		std::string* error_detail);

	::highbar::v1::AdminActionResult QueueAdminAction(
		const ::circuit::grpc::AdminCaller& caller,
		const ::highbar::v1::AdminAction& action,
		std::chrono::milliseconds timeout);

	// T011 — queue a fault transition from any thread. Worker-thread
	// callers (gRPC handlers, snapshot serializer) use this; the actual
	// side effects (log, unlink, health file) execute on the engine
	// thread the next time OnFrameTick runs. Idempotent.
	void RequestDisable(const std::string& subsystem,
	                    const std::string& reason,
	                    const std::string& detail);

	// T011 — engine-thread fault transition. Runs the 6 ordered side
	// effects from data-model.md §2. Engine-thread-only. Idempotent:
	// subsequent calls after the first are no-ops.
	void TransitionToDisabled(const std::string& subsystem,
	                          const std::string& reason,
	                          const std::string& detail);

	GatewayState State() const { return state_.load(std::memory_order_acquire); }
	bool IsDisabled() const { return State() == GatewayState::Disabled; }

	// 003-snapshot-arm-coverage — accessors for the snapshot tick.
	//
	// RequestSnapshot worker handlers call PendingSnapshotRequest() to
	// set the atomic flag; the engine thread drains it in OnFrameTick
	// at the top of every frame. Engine frame snapshot is exposed to
	// the RPC handler via CurrentFrame() (atomic; counter-based so no
	// lock is needed on the worker side).
	std::atomic<bool>& PendingSnapshotRequest() {
		return snapshot_tick_.PendingRequest();
	}
	std::uint32_t CurrentFrame() const {
		return current_frame_.load(std::memory_order_acquire);
	}

private:
	// Per-frame delta accumulator (T037/T038). Mutated on engine
	// thread only, so no lock needed for reads/writes from handlers.
	::highbar::v1::StateDelta current_frame_delta_;

	// Monotonic sequence across snapshot resets (data-model §2
	// invariants). Engine-thread only.
	std::uint64_t seq_ = 0;

	// KeepAlive quiet window: emit KeepAlive if no delta flushed for
	// this many frames. Default 1 second at 30Hz sim.
	static constexpr std::uint32_t kKeepAliveFrames = 30;
	std::uint32_t frames_since_last_flush_ = 0;

	// Shared/exclusive lock separating engine-thread delta publish
	// (exclusive) from worker-thread snapshot builds (shared). Writers
	// never block on gRPC I/O (research §3).
	std::shared_mutex state_mutex_;

	// Owning pointers. unique_ptr keeps gRPC headers out of this one.
	std::unique_ptr<grpc::AuthToken> token_;
	std::unique_ptr<grpc::AdminController> admin_controller_;
	std::unique_ptr<grpc::AdminService> admin_service_;
	std::unique_ptr<grpc::Counters> counters_;
	std::unique_ptr<grpc::SnapshotBuilder> snapshot_;
	std::unique_ptr<grpc::DeltaBus> delta_bus_;
	std::unique_ptr<grpc::RingBuffer> ring_;
	std::unique_ptr<grpc::CommandQueue> command_queue_;
	std::unique_ptr<grpc::OrderStateTracker> order_state_tracker_;
	std::unique_ptr<grpc::LiveControlState> live_control_state_;
	std::unique_ptr<grpc::FeatureLifetimeLedger> tactical_feature_lifetimes_;
	std::unique_ptr<grpc::MixedLifecycleQualification> mixed_lifecycle_qualification_;
	std::unique_ptr<grpc::QueuedCommand> mixed_lifecycle_held_command_;
	std::unique_ptr<grpc::HighBarService> service_;
	std::optional<grpc::TransportEndpoint> deferred_service_bind_endpoint_;
	bool service_bound_ = false;
	// Client-mode: plugin dials out to an external coordinator. See
	// specs/.../investigations/hello-rpc-deadline-exceeded.md for why
	// client-mode exists alongside the server-mode HighBarService.
	std::unique_ptr<grpc::CoordinatorClient> coordinator_client_;
	std::string coordinator_endpoint_;
	std::string coordinator_plugin_id_;
	std::string coordinator_engine_sha256_;
	bool coordinator_command_channel_started_ = false;
	bool coordinator_initial_snapshot_sent_ = false;
	static constexpr std::uint32_t kHeartbeatEveryNFrames = 30;
	std::uint32_t frame_counter_ = 0;
	std::uint32_t live_max_observation_age_ms_ = 2000;
	std::uint32_t live_max_reported_units_ = 64;
	std::string tactical_catalogue_id_;
	std::uint64_t tactical_catalogue_revision_ = 0;
	bool tactical_catalogue_complete_ = false;

	std::string bound_address_;

	// T009 — paths retained for TransitionToDisabled side effects.
	// `socket_path_` is empty for TCP; set to the UDS filesystem path
	// for UDS so TransitionToDisabled can unlink it.
	std::string socket_path_;
	std::string token_file_path_;
	std::string health_file_path_;

	// T009 — monotonic health state. Loaded with acquire semantics on
	// every hook entry; stored with release semantics on the last step
	// of TransitionToDisabled so readers see all side effects completed.
	std::atomic<GatewayState> state_{GatewayState::Healthy};

	// T011 — deferred fault request from worker threads. OnFrameTick
	// checks this at the top of each tick and runs TransitionToDisabled
	// on the engine thread if populated.
	struct PendingFault {
		std::string subsystem;
		std::string reason;
		std::string detail;
	};
	std::mutex pending_fault_mutex_;
	std::optional<PendingFault> pending_fault_;

	// T038 helper: serialize + publish current_frame_delta_.
	// Called from OnFrameTick under the exclusive lock.
	void FlushDelta(bool project_complete_world_state = false);
	// T039 helper: emit a KeepAlive StateUpdate on the bus + ring.
	void EmitKeepAlive();
	// T057 helper: drain CommandQueue, dispatch each via
	// CCircuitUnit::Cmd*. Engine-thread only.
	void DrainCommandQueue();
	bool BeginMixedLifecycleQualificationFrame();
	bool MaybeStartMixedLifecycleQualification(
		const grpc::QueuedCommand& command);
	void DrainAdminActionQueue();
	bool ApplyAdminAction(const ::highbar::v1::AdminAction& action);
	// Minimal InvokeCallback bridge. Worker threads enqueue callback
	// requests here; the engine thread resolves them at the top of the
	// frame before command dispatch.
	void DrainCallbackQueue();
	// Pull a deferred fault (if any) and run TransitionToDisabled on
	// the engine thread. Called at the top of OnFrameTick.
	void DrainPendingFault();
	// Client-mode startup is deferred out of Spring's spectator join
	// path, but can be safely started once gameplay callbacks begin.
	void EnsureLocalServiceBound(const char* reason);
	void EnsureCoordinatorClientStarted(const char* reason);
	void MaybeEmitInitialCoordinatorSnapshot(const char* reason);
	void BuildAndReportTacticalCatalogue();
	void BuildAndReportTacticalSnapshot(
		const ::highbar::v1::NativeObservationBasis& basis);

	// 003-snapshot-arm-coverage T011 — engine-thread snapshot
	// serializer + fan-out. Called from OnFrameTick when
	// snapshot_tick_.Pump() returns emit=true. Reuses the same
	// serializer/lock/ring/DeltaBus path that FlushDelta uses so the
	// snapshot emission inherits 002's Constitution V latency budget.
	void BroadcastSnapshot(std::uint32_t effective_cadence_frames);

	// 003-snapshot-arm-coverage — periodic-snapshot scheduler.
	::circuit::grpc::SnapshotTick snapshot_tick_;
	// Sparse damage/destroy events remain on the legacy delta stream, then
	// a complete current snapshot replaces their insufficient world facts.
	::circuit::grpc::StateUpdateOrder state_update_order_;

	// 003-snapshot-arm-coverage — atomic mirror of the current engine
	// frame. Written from OnFrameTick on the engine thread; read from
	// gRPC worker threads (RequestSnapshot handler). Monotonic but
	// allowed to lag the true engine frame by one tick on readers —
	// RequestSnapshot only uses it for the scheduled_frame return
	// value, which is an advisory correlation hint.
	std::atomic<std::uint32_t> current_frame_{0};

	struct PendingCallbackInvocation {
		::highbar::v1::CallbackRequest request;
		std::promise<CallbackInvocationResult> completion;
	};

	std::mutex pending_callbacks_mutex_;
	std::deque<std::shared_ptr<PendingCallbackInvocation>> pending_callbacks_;

	struct PendingAdminAction {
		::circuit::grpc::AdminCaller caller;
		::highbar::v1::AdminAction action;
		std::promise<::highbar::v1::AdminActionResult> completion;
	};

	std::mutex pending_admin_actions_mutex_;
	std::deque<std::shared_ptr<PendingAdminAction>> pending_admin_actions_;
};

}  // namespace circuit
