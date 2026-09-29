// SPDX-License-Identifier: GPL-2.0-only
//
// Client-mode gRPC surface: the plugin opens a channel to an external
// coordinator and sends heartbeats. Sidesteps the gRPC 1.76 EventEngine
// listener bug inside spring-headless (see
// specs/002-live-headless-e2e/investigations/hello-rpc-deadline-exceeded.md).

#pragma once

#include <grpcpp/grpcpp.h>

#include "highbar/coordinator.grpc.pb.h"
#include "highbar/live_control.grpc.pb.h"
#include "highbar/state.pb.h"
#include "highbar/commands.pb.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace circuit {
class CCircuitAI;
}  // namespace circuit

namespace circuit::grpc {

class CommandQueue;
class LiveControlState;

class CoordinatorClient {
public:
	// `endpoint` is a gRPC URI: "unix:/path" or "localhost:port".
	// `plugin_id` is a per-match identifier (e.g., the gameseed).
	CoordinatorClient(::circuit::CCircuitAI* ai,
	                  const std::string& endpoint,
	                  const std::string& plugin_id,
	                  const std::string& engine_sha256);
	~CoordinatorClient();

	CoordinatorClient(const CoordinatorClient&) = delete;
	CoordinatorClient& operator=(const CoordinatorClient&) = delete;

	// Call from OnFrameTick every N frames. Blocking-but-fast:
	// 250ms deadline. Returns true on OK reply, false on RPC error.
	// Engine-thread-only — no background threads.
	bool SendHeartbeat(std::uint32_t frame);

	// Phase B — enqueue a StateUpdate for the coordinator. Engine-thread
	// callers never perform gRPC I/O here; the push worker owns the
	// client-streaming RPC and drops oldest queued updates under
	// sustained backpressure.
	bool PushStateUpdate(const ::highbar::v1::StateUpdate& update);

	// Phase C — open the server-streaming command channel. Spawns a
	// background reader thread that atomically admits every CommandBatch
	// to the engine-thread CommandQueue. DrainCommandQueue picks commands
	// up at the top of each frame tick.
	void StartCommandChannel(CommandQueue* sink);
	void StartLiveChannels(CommandQueue* sink, LiveControlState* live_state);
	void SetLiveAdmissionObserver(
		std::function<void(const ::highbar::v1::LiveCommandBatch&)> observer) {
		live_admission_observer_ = std::move(observer);
	}
	void ReportLiveCapabilities(const ::highbar::v1::LiveNativeCapabilities& capabilities);
	void ReportLiveSnapshot(const ::highbar::v1::LiveSnapshotMetadata& snapshot);
	void ReportTacticalCatalogue(const ::highbar::v1::TacticalCataloguePage& page);
	void ReportTacticalSnapshot(const ::highbar::v1::TacticalSnapshotMetadata& snapshot);

	bool IsConnected() const { return connected_.load(std::memory_order_acquire); }
	std::uint64_t OkCount() const { return ok_count_.load(std::memory_order_acquire); }
	std::uint64_t ErrCount() const { return err_count_.load(std::memory_order_acquire); }
	std::uint64_t PushedCount() const { return pushed_count_.load(std::memory_order_acquire); }

private:
	void OpenPushStateStream();   // lazy; idempotent.
	void ClosePushStateStream();  // called from dtor; safe if stream never opened.
	void PushWorkerLoop();

	::circuit::CCircuitAI* ai_;
	std::string endpoint_;
	std::string plugin_id_;
	std::string engine_sha256_;
	std::shared_ptr<::grpc::Channel> channel_;
	std::unique_ptr<::highbar::v1::HighBarCoordinator::Stub> stub_;
	std::shared_ptr<::grpc::Channel> cmd_channel_;
	std::unique_ptr<::highbar::v1::HighBarCoordinator::Stub> cmd_stub_;
	std::atomic<bool> connected_{false};
	std::atomic<std::uint64_t> ok_count_{0};
	std::atomic<std::uint64_t> err_count_{0};

	// PushState stream — owned by push_thread_ so engine-thread callers
	// never block on coordinator I/O.
	std::shared_ptr<::grpc::ClientContext> push_ctx_;
	::highbar::v1::PushAck push_ack_;
	std::unique_ptr<::grpc::ClientWriter<::highbar::v1::StateUpdate>> push_writer_;
	std::atomic<bool> push_stream_open_{false};
	std::atomic<std::uint64_t> pushed_count_{0};
	std::thread push_thread_;
	std::atomic<bool> push_stopping_{false};
	std::mutex push_queue_mutex_;
	std::condition_variable push_queue_cv_;
	std::deque<::highbar::v1::StateUpdate> push_queue_;
	std::atomic<std::uint64_t> push_dropped_count_{0};
	std::mutex push_ctx_mutex_;
	static constexpr std::size_t kMaxQueuedPushUpdates = 256;

	// OpenCommandChannel reader — background thread. Owned lifetime
	// flows: StartCommandChannel starts it, dtor cancels + joins.
	void CommandReaderLoop(CommandQueue* sink);
	bool ReportCommandBatchResult(
		const std::string& channel_incarnation,
		const ::highbar::v1::CommandBatch& batch,
		const struct CommandBatchResult& admission);
	std::unique_ptr<::grpc::ClientContext> cmd_ctx_;
	std::thread cmd_thread_;
	std::atomic<bool> cmd_stopping_{false};
	std::atomic<std::uint64_t> cmd_batches_received_{0};
	std::atomic<std::uint64_t> cmd_batches_accepted_{0};
	std::atomic<std::uint64_t> cmd_batches_rejected_invalid_{0};
	std::atomic<std::uint64_t> cmd_batches_rejected_full_{0};
	std::atomic<std::uint64_t> cmd_commands_received_{0};

	void LiveControlReaderLoop(LiveControlState* state);
	void LiveCommandReaderLoop(CommandQueue* sink, LiveControlState* state);
	void LiveReportWorkerLoop();
	bool SendLiveControlAck(const ::highbar::v1::LiveControlAckReport& report);
	void QueueLiveStateReport(::highbar::v1::LiveStateReport report);
	std::shared_ptr<::grpc::Channel> live_channel_;
	std::unique_ptr<::highbar::v1::HighBarLiveControl::Stub> live_stub_;
	std::thread live_control_thread_;
	std::thread live_command_thread_;
	std::thread live_report_thread_;
	std::atomic<bool> live_stopping_{false};
	std::mutex live_context_mutex_;
	std::shared_ptr<::grpc::ClientContext> live_control_ctx_;
	std::shared_ptr<::grpc::ClientContext> live_command_ctx_;
	std::mutex live_report_mutex_;
	std::condition_variable live_report_cv_;
	std::deque<::highbar::v1::LiveStateReport> live_reports_;
	std::atomic<std::uint64_t> live_report_sequence_{0};
	LiveControlState* live_state_ = nullptr;
	std::function<void(const ::highbar::v1::LiveCommandBatch&)> live_admission_observer_;
	static constexpr std::size_t kMaxQueuedLiveReports = 64;
};

}  // namespace circuit::grpc
