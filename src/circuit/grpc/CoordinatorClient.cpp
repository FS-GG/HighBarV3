// SPDX-License-Identifier: GPL-2.0-only

#include "grpc/CoordinatorClient.h"
#include "grpc/CommandQueue.h"
#include "grpc/LiveControlState.h"
#include "grpc/GrpcLog.h"
#include "grpc/SchemaVersion.h"

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>
#include <utility>

namespace {

std::mutex& CoordinatorTraceMutex() {
	static std::mutex m;
	return m;
}

void AppendCoordinatorTrace(const std::string& plugin_id, const std::string& message) {
	const char* path = std::getenv("HIGHBAR_COORDINATOR_TRACE");
	if (path == nullptr || path[0] == '\0') return;

	std::lock_guard<std::mutex> lock(CoordinatorTraceMutex());
	FILE* f = std::fopen(path, "a");
	if (f == nullptr) return;

	const auto now = std::chrono::system_clock::now();
	const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
		now.time_since_epoch()).count();
	std::fprintf(f, "%lld plugin=%s %s\n",
	             static_cast<long long>(micros),
	             plugin_id.c_str(),
	             message.c_str());
	std::fclose(f);
}

const char* ConnectivityStateName(::grpc_connectivity_state state) {
	switch (state) {
	case GRPC_CHANNEL_IDLE:
		return "IDLE";
	case GRPC_CHANNEL_CONNECTING:
		return "CONNECTING";
	case GRPC_CHANNEL_READY:
		return "READY";
	case GRPC_CHANNEL_TRANSIENT_FAILURE:
		return "TRANSIENT_FAILURE";
	case GRPC_CHANNEL_SHUTDOWN:
		return "SHUTDOWN";
	}
	return "UNKNOWN";
}

const char* CommandBatchAdmissionStatusName(
		circuit::grpc::CommandBatchAdmissionStatus status) {
	using circuit::grpc::CommandBatchAdmissionStatus;
	switch (status) {
	case CommandBatchAdmissionStatus::kAccepted:
		return "accepted";
	case CommandBatchAdmissionStatus::kInvalidEmpty:
		return "invalid-empty";
	case CommandBatchAdmissionStatus::kInvalidOversized:
		return "invalid-oversized";
	case CommandBatchAdmissionStatus::kInvalidTarget:
		return "invalid-target";
	case CommandBatchAdmissionStatus::kInvalidBatchSequence:
		return "invalid-batch-sequence";
	case CommandBatchAdmissionStatus::kInvalidCorrelation:
		return "invalid-correlation";
	case CommandBatchAdmissionStatus::kDuplicate:
		return "duplicate";
	case CommandBatchAdmissionStatus::kQueueFull:
		return "queue-full";
	}
	return "unknown";
}

const char* LiveStateDispositionName(::highbar::v1::LiveStateReportDisposition disposition) {
	switch (disposition) {
	case ::highbar::v1::LIVE_STATE_REPORT_RECORDED: return "recorded";
	case ::highbar::v1::LIVE_STATE_REPORT_DUPLICATE: return "duplicate";
	case ::highbar::v1::LIVE_STATE_REPORT_STALE: return "stale";
	case ::highbar::v1::LIVE_STATE_REPORT_REFUSED: return "refused";
	case ::highbar::v1::LIVE_STATE_REPORT_DISPOSITION_UNSPECIFIED: return "unspecified";
	default: return "unknown";
	}
}

std::string NewChannelIncarnation(const std::string& plugin_id) {
	static std::atomic<std::uint64_t> counter{0};
	const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
	return plugin_id + "-" + std::to_string(ticks) + "-"
		+ std::to_string(counter.fetch_add(1, std::memory_order_relaxed) + 1);
}

::highbar::v1::CommandIssueCode AdmissionIssueCode(
		circuit::grpc::CommandBatchAdmissionStatus status) {
	using circuit::grpc::CommandBatchAdmissionStatus;
	switch (status) {
	case CommandBatchAdmissionStatus::kInvalidEmpty:
		return ::highbar::v1::EMPTY_COMMAND;
	case CommandBatchAdmissionStatus::kInvalidOversized:
		return ::highbar::v1::TOO_MANY_COMMANDS;
	case CommandBatchAdmissionStatus::kInvalidTarget:
		return ::highbar::v1::INVALID_TARGET_UNIT;
	case CommandBatchAdmissionStatus::kInvalidBatchSequence:
		return ::highbar::v1::STALE_OR_DUPLICATE_BATCH_SEQ;
	case CommandBatchAdmissionStatus::kInvalidCorrelation:
		return ::highbar::v1::MISSING_CLIENT_COMMAND_ID;
	case CommandBatchAdmissionStatus::kDuplicate:
		return ::highbar::v1::STALE_OR_DUPLICATE_BATCH_SEQ;
	case CommandBatchAdmissionStatus::kQueueFull:
		return ::highbar::v1::QUEUE_FULL;
	case CommandBatchAdmissionStatus::kAccepted:
		return ::highbar::v1::COMMAND_ISSUE_CODE_UNSPECIFIED;
	}
	return ::highbar::v1::COMMAND_ISSUE_CODE_UNSPECIFIED;
}

}  // namespace

namespace circuit::grpc {

CoordinatorClient::CoordinatorClient(::circuit::CCircuitAI* ai,
                                       const std::string& endpoint,
                                       const std::string& plugin_id,
                                       const std::string& engine_sha256)
	: ai_(ai)
	, endpoint_(endpoint)
	, plugin_id_(plugin_id)
	, engine_sha256_(engine_sha256)
	, channel_(::grpc::CreateChannel(endpoint, ::grpc::InsecureChannelCredentials()))
	, stub_(::highbar::v1::HighBarCoordinator::NewStub(channel_)) {
	cmd_channel_ = ::grpc::CreateChannel(endpoint, ::grpc::InsecureChannelCredentials());
	cmd_stub_ = ::highbar::v1::HighBarCoordinator::NewStub(cmd_channel_);
	live_channel_ = ::grpc::CreateChannel(endpoint, ::grpc::InsecureChannelCredentials());
	live_stub_ = ::highbar::v1::HighBarLiveControl::NewStub(live_channel_);
	AppendCoordinatorTrace(plugin_id_, "ctor connected");
	LogConnect(ai_, plugin_id_, endpoint_, "client-mode");
	push_thread_ = std::thread(&CoordinatorClient::PushWorkerLoop, this);
}

CoordinatorClient::~CoordinatorClient() {
	AppendCoordinatorTrace(plugin_id_, "dtor begin");
	live_stopping_.store(true, std::memory_order_release);
	live_report_cv_.notify_all();
	{
		std::lock_guard<std::mutex> lock(live_context_mutex_);
		if (live_control_ctx_) live_control_ctx_->TryCancel();
		if (live_command_ctx_) live_command_ctx_->TryCancel();
	}
	if (live_control_thread_.joinable()) live_control_thread_.join();
	if (live_command_thread_.joinable()) live_command_thread_.join();
	if (live_report_thread_.joinable()) live_report_thread_.join();
	push_stopping_.store(true, std::memory_order_release);
	push_queue_cv_.notify_all();
	{
		std::lock_guard<std::mutex> lock(push_ctx_mutex_);
		if (push_ctx_) push_ctx_->TryCancel();
	}
	if (push_thread_.joinable()) push_thread_.join();

	// Cancel + join the command reader if running.
	cmd_stopping_.store(true, std::memory_order_release);
	if (cmd_ctx_) cmd_ctx_->TryCancel();
	if (cmd_thread_.joinable()) cmd_thread_.join();
	AppendCoordinatorTrace(plugin_id_, "dtor end");
}

void CoordinatorClient::StartLiveChannels(CommandQueue* sink, LiveControlState* state) {
	if (sink == nullptr || state == nullptr || live_control_thread_.joinable()) return;
	live_state_ = state;
	state->ReplaceChannels(
		NewChannelIncarnation(plugin_id_ + "-live-command"),
		NewChannelIncarnation(plugin_id_ + "-live-control"));
	live_control_thread_ = std::thread(&CoordinatorClient::LiveControlReaderLoop, this, state);
	live_command_thread_ = std::thread(&CoordinatorClient::LiveCommandReaderLoop, this, sink, state);
	live_report_thread_ = std::thread(&CoordinatorClient::LiveReportWorkerLoop, this);
}

bool CoordinatorClient::SendLiveControlAck(const ::highbar::v1::LiveControlAckReport& report) {
	for (int attempt = 0; attempt < 2; ++attempt) {
		::grpc::ClientContext ctx;
		ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(250));
		::highbar::v1::LiveControlAckResponse response;
		const auto status = live_stub_->ReportLiveControlAck(&ctx, report, &response);
		if (status.ok()) return true;
		if (live_stopping_.load(std::memory_order_acquire)) return false;
	}
	return false;
}

void CoordinatorClient::LiveControlReaderLoop(LiveControlState* state) {
	std::uint32_t backoff_ms = 200;
	while (!live_stopping_.load(std::memory_order_acquire)) {
		auto ctx = std::make_shared<::grpc::ClientContext>();
		ctx->set_deadline(std::chrono::system_clock::now() + std::chrono::minutes(60));
		{
			std::lock_guard<std::mutex> lock(live_context_mutex_); live_control_ctx_ = ctx;
		}
		::highbar::v1::LiveControlSubscribe sub;
		sub.set_plugin_id(plugin_id_); sub.set_schema_version(::highbar::v1::kSchemaVersion);
		sub.set_protocol(::highbar::v1::LIVE_CONTROL_PROTOCOL_TACTICAL_V1);
		sub.set_process_incarnation(state->ProcessIncarnation());
		sub.set_match_incarnation(state->MatchIncarnation());
		sub.set_command_channel_incarnation(state->CommandChannelIncarnation());
		sub.set_control_channel_incarnation(state->ControlChannelIncarnation());
		auto reader = live_stub_->OpenLiveControlChannel(ctx.get(), sub);
		if (reader) {
			backoff_ms = 200; ::highbar::v1::LiveControlDirective directive;
			while (reader->Read(&directive)) {
				const auto ack = state->ApplyDirective(directive);
				(void)SendLiveControlAck(ack);
			}
			(void)reader->Finish();
		}
		{
			std::lock_guard<std::mutex> lock(live_context_mutex_); live_control_ctx_.reset();
		}
		if (!live_stopping_.load(std::memory_order_acquire)) {
			// Losing the priority stream immediately invalidates its authority
			// generation. Rotate both paired incarnations before backoff and
			// cancel the gameplay stream so no old-binding work can arrive or
			// dispatch while the replacement control stream is pending.
			state->ReplaceChannels(
				NewChannelIncarnation(plugin_id_ + "-live-command"),
				NewChannelIncarnation(plugin_id_ + "-live-control"));
			std::lock_guard<std::mutex> lock(live_context_mutex_);
			if (live_command_ctx_) live_command_ctx_->TryCancel();
		}
		for (std::uint32_t elapsed=0; elapsed<backoff_ms && !live_stopping_.load(); elapsed+=50)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		backoff_ms = std::min<std::uint32_t>(backoff_ms*2, 5000);
	}
}

void CoordinatorClient::LiveCommandReaderLoop(CommandQueue* sink, LiveControlState* state) {
	std::uint32_t backoff_ms = 200;
	while (!live_stopping_.load(std::memory_order_acquire)) {
		auto ctx = std::make_shared<::grpc::ClientContext>();
		ctx->set_deadline(std::chrono::system_clock::now() + std::chrono::minutes(60));
		{
			std::lock_guard<std::mutex> lock(live_context_mutex_); live_command_ctx_ = ctx;
		}
		::highbar::v1::LiveCommandSubscribe sub;
		sub.set_plugin_id(plugin_id_); sub.set_schema_version(::highbar::v1::kSchemaVersion);
		sub.set_protocol(::highbar::v1::LIVE_CONTROL_PROTOCOL_TACTICAL_V1);
		sub.mutable_binding()->set_plugin_id(plugin_id_);
		sub.mutable_binding()->set_process_incarnation(state->ProcessIncarnation());
		sub.mutable_binding()->set_match_incarnation(state->MatchIncarnation());
		sub.mutable_binding()->set_command_channel_incarnation(state->CommandChannelIncarnation());
		sub.mutable_binding()->set_control_channel_incarnation(state->ControlChannelIncarnation());
		auto reader = live_stub_->OpenLiveCommandChannel(ctx.get(), sub);
		if (reader) {
			backoff_ms = 200; ::highbar::v1::LiveCommandBatch live;
			while (reader->Read(&live)) {
				const auto admission = AdmitLiveCommandBatch(*sink, live, plugin_id_ + "-live", *state);
				if (admission.accepted() && live_admission_observer_) {
					live_admission_observer_(live);
				}
				(void)ReportCommandBatchResult(live.binding().command_channel_incarnation(), live.batch(), admission);
			}
			(void)reader->Finish();
		}
		{
			std::lock_guard<std::mutex> lock(live_context_mutex_); live_command_ctx_.reset();
		}
		for (std::uint32_t elapsed=0; elapsed<backoff_ms && !live_stopping_.load(); elapsed+=50)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		backoff_ms = std::min<std::uint32_t>(backoff_ms*2, 5000);
	}
}

void CoordinatorClient::QueueLiveStateReport(::highbar::v1::LiveStateReport report) {
	if (!live_state_ || live_stopping_.load(std::memory_order_acquire)) return;
	auto* reporter = report.mutable_reporter();
	reporter->set_plugin_id(plugin_id_); reporter->set_schema_version(::highbar::v1::kSchemaVersion);
	reporter->set_protocol(::highbar::v1::LIVE_CONTROL_PROTOCOL_TACTICAL_V1);
	reporter->set_process_incarnation(live_state_->ProcessIncarnation());
	reporter->set_match_incarnation(live_state_->MatchIncarnation());
	reporter->set_state_channel_incarnation(live_state_->StateChannelIncarnation());
	report.set_report_sequence(live_report_sequence_.fetch_add(1) + 1);
	{
		std::lock_guard<std::mutex> lock(live_report_mutex_);
		if (live_reports_.size() >= kMaxQueuedLiveReports) {
			// Keep the capability advertisement under snapshot backpressure:
			// live arm is impossible until the broker has recorded it. Snapshot
			// reports are periodic, so evicting the oldest snapshot retains the
			// newest bounded view without blocking the engine thread.
			auto is_periodic_snapshot = [](const auto& queued) {
				return queued.body_case() == ::highbar::v1::LiveStateReport::kSnapshot
					|| queued.body_case() == ::highbar::v1::LiveStateReport::kTacticalSnapshot;
			};
			auto victim = std::find_if(live_reports_.begin(), live_reports_.end(),
				[](const auto& queued) {
					return queued.body_case() == ::highbar::v1::LiveStateReport::kSnapshot
						|| queued.body_case() == ::highbar::v1::LiveStateReport::kTacticalSnapshot;
				});
			if (victim == live_reports_.end()) {
				if (is_periodic_snapshot(report)) return;
				live_reports_.pop_front();
			} else {
				live_reports_.erase(victim);
			}
		}
		live_reports_.push_back(std::move(report));
	}
	live_report_cv_.notify_one();
}

void CoordinatorClient::ReportLiveCapabilities(const ::highbar::v1::LiveNativeCapabilities& c) {
	::highbar::v1::LiveStateReport report; *report.mutable_capabilities() = c; QueueLiveStateReport(std::move(report));
}
void CoordinatorClient::ReportLiveSnapshot(const ::highbar::v1::LiveSnapshotMetadata& s) {
	::highbar::v1::LiveStateReport report; *report.mutable_snapshot() = s; QueueLiveStateReport(std::move(report));
}
void CoordinatorClient::ReportTacticalCatalogue(const ::highbar::v1::TacticalCataloguePage& p) {
	::highbar::v1::LiveStateReport report;
	*report.mutable_tactical_catalogue() = p;
	QueueLiveStateReport(std::move(report));
}
void CoordinatorClient::ReportTacticalSnapshot(const ::highbar::v1::TacticalSnapshotMetadata& s) {
	::highbar::v1::LiveStateReport report;
	*report.mutable_tactical_snapshot() = s;
	QueueLiveStateReport(std::move(report));
}

void CoordinatorClient::LiveReportWorkerLoop() {
	while (!live_stopping_.load(std::memory_order_acquire)) {
		::highbar::v1::LiveStateReport report;
		{
			std::unique_lock<std::mutex> lock(live_report_mutex_);
			live_report_cv_.wait(lock, [&]{ return live_stopping_.load() || !live_reports_.empty(); });
			if (live_stopping_.load()) break;
			report = std::move(live_reports_.front()); live_reports_.pop_front();
		}
		std::uint32_t backoff_ms = 100;
		while (!live_stopping_.load(std::memory_order_acquire)) {
			::grpc::ClientContext ctx; ctx.set_deadline(std::chrono::system_clock::now()+std::chrono::milliseconds(750));
			::highbar::v1::LiveStateReportAck ack;
			const auto status = live_stub_->ReportLiveState(&ctx, report, &ack);
			if (status.ok()) {
				AppendCoordinatorTrace(plugin_id_,
					"live state ack seq=" + std::to_string(ack.report_sequence())
					+ " disposition=" + LiveStateDispositionName(ack.disposition())
					+ " body=" + std::to_string(static_cast<int>(report.body_case())));
				break;
			}
			AppendCoordinatorTrace(plugin_id_,
				"live state rpc failed seq=" + std::to_string(report.report_sequence())
				+ " code=" + std::to_string(status.error_code())
				+ " msg=" + status.error_message());
			for (std::uint32_t elapsed=0; elapsed<backoff_ms && !live_stopping_.load(); elapsed+=50)
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			backoff_ms=std::min<std::uint32_t>(backoff_ms*2,5000);
		}
	}
}

bool CoordinatorClient::SendHeartbeat(std::uint32_t frame) {
	AppendCoordinatorTrace(
		plugin_id_,
		"heartbeat attempt frame=" + std::to_string(frame)
		+ " hb_state=" + ConnectivityStateName(channel_->GetState(false))
		+ " cmd_state=" + ConnectivityStateName(cmd_channel_->GetState(false))
		+ " pushed=" + std::to_string(pushed_count_.load(std::memory_order_relaxed))
		+ " cmd_batches=" + std::to_string(cmd_batches_received_.load(std::memory_order_relaxed))
		+ " cmd_batches_accepted="
		+ std::to_string(cmd_batches_accepted_.load(std::memory_order_relaxed))
		+ " cmd_batches_invalid="
		+ std::to_string(cmd_batches_rejected_invalid_.load(std::memory_order_relaxed))
		+ " cmd_batches_full="
		+ std::to_string(cmd_batches_rejected_full_.load(std::memory_order_relaxed))
		+ " cmd_commands_accepted="
		+ std::to_string(cmd_commands_received_.load(std::memory_order_relaxed)));
	::highbar::v1::HeartbeatRequest req;
	req.set_plugin_id(plugin_id_);
	req.set_frame(frame);
	req.set_engine_sha256(engine_sha256_);
	req.set_schema_version(::highbar::v1::kSchemaVersion);

	::highbar::v1::HeartbeatResponse resp;
	::grpc::ClientContext ctx;
	ctx.set_deadline(std::chrono::system_clock::now()
	                 + std::chrono::milliseconds(250));

	const ::grpc::Status status = stub_->Heartbeat(&ctx, req, &resp);
	if (status.ok()) {
		connected_.store(true, std::memory_order_release);
		ok_count_.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	AppendCoordinatorTrace(plugin_id_,
	                      "heartbeat failed code=" + std::to_string(status.error_code())
	                      + " msg=" + status.error_message());
	connected_.store(false, std::memory_order_release);
	err_count_.fetch_add(1, std::memory_order_relaxed);
	// Rate-limit: only log the first failure per streak.
	const std::uint64_t errs = err_count_.load(std::memory_order_relaxed);
	if (errs == 1 || (errs & (errs - 1)) == 0) {
		// Powers of two: log 1, 2, 4, 8, 16 etc.
		LogError(ai_, "CoordinatorClient",
		         "Heartbeat failed code=" + std::to_string(status.error_code())
		         + " msg=" + status.error_message()
		         + " err_streak=" + std::to_string(errs));
	}
	return false;
}

void CoordinatorClient::OpenPushStateStream() {
	if (push_stream_open_.load(std::memory_order_acquire)) return;
	auto ctx = std::make_shared<::grpc::ClientContext>();
	// Unlike unary Heartbeat, the stream is long-lived. We only set a
	// generous per-write deadline (60 min); a hard deadline would have
	// to be refreshed every match.
	ctx->set_deadline(std::chrono::system_clock::now()
	                  + std::chrono::minutes(60));
	{
		std::lock_guard<std::mutex> lock(push_ctx_mutex_);
		push_ctx_ = ctx;
	}
	if (push_stopping_.load(std::memory_order_acquire)) {
		ctx->TryCancel();
	}
	push_writer_ = stub_->PushState(ctx.get(), &push_ack_);
	if (!push_writer_) {
		std::lock_guard<std::mutex> lock(push_ctx_mutex_);
		push_ctx_.reset();
		return;
	}
	push_stream_open_.store(true, std::memory_order_release);
	AppendCoordinatorTrace(plugin_id_, "push open");
	LogConnect(ai_, plugin_id_, endpoint_, "client-mode-push-stream");
}

void CoordinatorClient::ClosePushStateStream() {
	if (!push_stream_open_.exchange(false, std::memory_order_acq_rel)) return;
	if (push_writer_) {
		AppendCoordinatorTrace(plugin_id_, "push close begin");
		push_writer_->WritesDone();
		::grpc::Status status = push_writer_->Finish();
		if (status.ok()) {
			AppendCoordinatorTrace(plugin_id_,
			                      "push close ok msgs_rx="
			                      + std::to_string(push_ack_.messages_received())
			                      + " max_seq=" + std::to_string(push_ack_.max_seq_seen()));
			LogError(ai_, "CoordinatorClient",
			         "PushState closed ok msgs_rx="
			         + std::to_string(push_ack_.messages_received())
			         + " max_seq=" + std::to_string(push_ack_.max_seq_seen()));
		} else {
			AppendCoordinatorTrace(plugin_id_,
			                      "push close err code="
			                      + std::to_string(status.error_code())
			                      + " msg=" + status.error_message());
			LogError(ai_, "CoordinatorClient",
			         "PushState closed err code="
			         + std::to_string(status.error_code())
			         + " msg=" + status.error_message());
		}
	}
	push_writer_.reset();
	{
		std::lock_guard<std::mutex> lock(push_ctx_mutex_);
		push_ctx_.reset();
	}
}

void CoordinatorClient::StartCommandChannel(CommandQueue* sink) {
	if (sink == nullptr) return;
	if (cmd_thread_.joinable()) return;  // already started
	AppendCoordinatorTrace(plugin_id_, "cmd thread start requested");
	cmd_thread_ = std::thread(&CoordinatorClient::CommandReaderLoop, this, sink);
}

void CoordinatorClient::CommandReaderLoop(CommandQueue* sink) {
	using ::highbar::v1::CommandBatch;
	using ::highbar::v1::CommandChannelSubscribe;

	// Long-lived read loop. If the server closes the stream (for
	// example during coordinator restart), we attempt a reconnect with
	// exponential backoff up to 5 s. Stops when cmd_stopping_ is set.
	std::uint32_t backoff_ms = 200;
	while (!cmd_stopping_.load(std::memory_order_acquire)) {
		const std::string channel_incarnation = NewChannelIncarnation(plugin_id_);
		AppendCoordinatorTrace(plugin_id_,
		                      "cmd loop connect attempt backoff_ms="
		                      + std::to_string(backoff_ms));
		cmd_ctx_ = std::make_unique<::grpc::ClientContext>();
		cmd_ctx_->set_deadline(std::chrono::system_clock::now()
		                       + std::chrono::minutes(60));

		CommandChannelSubscribe sub;
		sub.set_plugin_id(plugin_id_);
		sub.set_schema_version(::highbar::v1::kSchemaVersion);
		sub.set_admission_result_protocol(
			::highbar::v1::ADMISSION_RESULT_PROTOCOL_CORRELATED_V1);
		sub.set_channel_incarnation(channel_incarnation);

		auto reader = cmd_stub_->OpenCommandChannel(cmd_ctx_.get(), sub);
		if (!reader) {
			AppendCoordinatorTrace(plugin_id_, "cmd loop null reader");
			LogError(ai_, "CoordinatorClient",
			         "OpenCommandChannel returned null reader");
		} else {
			AppendCoordinatorTrace(plugin_id_, "cmd loop reader opened");
			LogConnect(ai_, plugin_id_, endpoint_, "client-mode-cmd-channel");
			CommandBatch batch;
			while (reader->Read(&batch)) {
				AppendCoordinatorTrace(plugin_id_,
				                      "cmd batch read seq="
				                      + std::to_string(batch.batch_seq())
				                      + " ncmds=" + std::to_string(batch.commands_size()));
				cmd_batches_received_.fetch_add(1, std::memory_order_relaxed);
				const auto admission = AdmitCommandBatch(
					*sink, batch, plugin_id_ + "-cmd-ch", channel_incarnation);
				if (admission.accepted()) {
					cmd_batches_accepted_.fetch_add(1, std::memory_order_relaxed);
					cmd_commands_received_.fetch_add(
						admission.accepted_command_count, std::memory_order_relaxed);
				} else if (admission.status
				           == CommandBatchAdmissionStatus::kQueueFull) {
					cmd_batches_rejected_full_.fetch_add(1, std::memory_order_relaxed);
				} else {
					cmd_batches_rejected_invalid_.fetch_add(1, std::memory_order_relaxed);
				}
				const std::string outcome =
					"cmd batch admission seq=" + std::to_string(batch.batch_seq())
					+ " correlation="
					+ std::to_string(batch.has_client_command_id()
					                 ? batch.client_command_id() : 0)
					+ " ncmds=" + std::to_string(batch.commands_size())
					+ " status=" + CommandBatchAdmissionStatusName(admission.status);
				AppendCoordinatorTrace(plugin_id_, outcome);
				if (!admission.accepted()) {
					LogError(ai_, "CoordinatorClient", outcome);
				}
				if (!ReportCommandBatchResult(
						channel_incarnation, batch, admission)) {
					LogError(ai_, "CoordinatorClient",
					         "native admission result was not acknowledged seq="
					         + std::to_string(batch.batch_seq()));
				}
			}
			AppendCoordinatorTrace(plugin_id_, "cmd read loop ended");
			LogDisconnect(ai_, plugin_id_, "cmd-channel-read-closed");
			::grpc::Status st = reader->Finish();
			if (st.ok()) {
				AppendCoordinatorTrace(plugin_id_, "cmd finish ok");
				LogDisconnect(ai_, plugin_id_, "cmd-channel-finish-ok");
			} else if (st.error_code() == ::grpc::StatusCode::CANCELLED) {
				AppendCoordinatorTrace(plugin_id_, "cmd finish cancelled");
				LogDisconnect(ai_, plugin_id_, "cmd-channel-finish-cancelled");
				break;  // shutdown — stop the loop
			} else {
				AppendCoordinatorTrace(plugin_id_,
				                      "cmd finish err code="
				                      + std::to_string(st.error_code())
				                      + " msg=" + st.error_message());
				LogDisconnect(ai_, plugin_id_,
				              "cmd-channel-finish-code="
				              + std::to_string(st.error_code()));
				LogError(ai_, "CoordinatorClient",
				         "cmd channel err code="
				         + std::to_string(st.error_code())
				         + " msg=" + st.error_message());
			}
		}

		if (cmd_stopping_.load(std::memory_order_acquire)) break;
		AppendCoordinatorTrace(plugin_id_,
		                      "cmd loop sleeping backoff_ms="
		                      + std::to_string(backoff_ms));
		// Sleep backoff before retry (but check stopping again).
		for (std::uint32_t i = 0;
		     i < backoff_ms && !cmd_stopping_.load(std::memory_order_acquire);
		     i += 50) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		backoff_ms = std::min<std::uint32_t>(backoff_ms * 2, 5000);
	}
	AppendCoordinatorTrace(plugin_id_, "cmd loop exit");
}

bool CoordinatorClient::ReportCommandBatchResult(
		const std::string& channel_incarnation,
		const ::highbar::v1::CommandBatch& batch,
		const CommandBatchResult& admission) {
	::highbar::v1::CommandBatchResultReport request;
	request.set_plugin_id(plugin_id_);
	request.set_channel_incarnation(channel_incarnation);
	request.set_schema_version(::highbar::v1::kSchemaVersion);
	auto* result = request.mutable_result();
	result->set_batch_seq(batch.batch_seq());
	result->set_client_command_id(
		batch.has_client_command_id() ? batch.client_command_id() : 0);
	result->set_accepted_command_count(
		static_cast<std::uint32_t>(admission.accepted_command_count));
	result->set_mode(::highbar::v1::VALIDATION_MODE_STRICT);
	if (admission.accepted()) {
		result->set_status(::highbar::v1::COMMAND_BATCH_ACCEPTED);
	} else if (admission.status == CommandBatchAdmissionStatus::kQueueFull) {
		result->set_status(::highbar::v1::COMMAND_BATCH_REJECTED_QUEUE_FULL);
	} else {
		result->set_status(::highbar::v1::COMMAND_BATCH_REJECTED_INVALID);
	}
	if (!admission.accepted()) {
		auto* issue = result->add_issues();
		issue->set_code(AdmissionIssueCode(admission.status));
		issue->set_detail(CommandBatchAdmissionStatusName(admission.status));
		issue->set_batch_seq(batch.batch_seq());
		issue->set_client_command_id(result->client_command_id());
		issue->set_retry_hint(
			admission.status == CommandBatchAdmissionStatus::kQueueFull
				? ::highbar::v1::RETRY_AFTER_QUEUE_DRAINS
				: ::highbar::v1::RETRY_NEVER);
	}

	// The command reader is already a background thread. Bound every network
	// attempt and retry only the exact idempotent report; commands themselves
	// are never replayed.
	for (int attempt = 0; attempt < 2; ++attempt) {
		::grpc::ClientContext context;
		context.set_deadline(std::chrono::system_clock::now()
		                     + std::chrono::milliseconds(750));
		::highbar::v1::CommandBatchResultReportAck ack;
		const auto status = cmd_stub_->ReportCommandBatchResult(
			&context, request, &ack);
		if (status.ok()) {
			AppendCoordinatorTrace(
				plugin_id_, "admission result ack seq="
				+ std::to_string(batch.batch_seq()) + " correlation="
				+ std::to_string(result->client_command_id()) + " disposition="
				+ std::to_string(ack.disposition()));
			return true;
		}
		if (status.error_code() == ::grpc::StatusCode::UNIMPLEMENTED
		    || status.error_code() == ::grpc::StatusCode::FAILED_PRECONDITION
		    || cmd_stopping_.load(std::memory_order_acquire)) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return false;
}

void CoordinatorClient::PushWorkerLoop() {
	while (!push_stopping_.load(std::memory_order_acquire)) {
		::highbar::v1::StateUpdate update;
		{
			std::unique_lock<std::mutex> lock(push_queue_mutex_);
			push_queue_cv_.wait(lock, [this] {
				return push_stopping_.load(std::memory_order_acquire)
					|| !push_queue_.empty();
			});
			if (push_stopping_.load(std::memory_order_acquire)) break;
			update = std::move(push_queue_.front());
			push_queue_.pop_front();
		}

		if (push_stopping_.load(std::memory_order_acquire)) break;
		if (!push_stream_open_.load(std::memory_order_acquire)) {
			OpenPushStateStream();
		}
		if (!push_writer_) continue;
		if (push_stopping_.load(std::memory_order_acquire)) break;

		// Constitution V instrumentation: stamp CLOCK_MONOTONIC_ns at the
		// moment the push worker hands the frame to gRPC.
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		if (update.send_monotonic_ns() == 0) {
			update.set_send_monotonic_ns(
				static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL
				+ static_cast<std::uint64_t>(ts.tv_nsec));
		}

		// gRPC ClientWriter::Write can block under backpressure, so this
		// path intentionally runs off the Spring engine thread.
		if (!push_writer_->Write(update)) {
			const std::uint64_t pushed = pushed_count_.load();
			AppendCoordinatorTrace(plugin_id_,
			                      "push write failed after="
			                      + std::to_string(pushed));
			ClosePushStateStream();
			LogError(ai_, "CoordinatorClient",
			         "PushState stream broken after "
			         + std::to_string(pushed) + " messages");
			continue;
		}
		pushed_count_.fetch_add(1, std::memory_order_relaxed);
	}

	ClosePushStateStream();
}

bool CoordinatorClient::PushStateUpdate(const ::highbar::v1::StateUpdate& update) {
	if (push_stopping_.load(std::memory_order_acquire)) return false;
	std::uint64_t dropped = 0;
	{
		std::lock_guard<std::mutex> lock(push_queue_mutex_);
		if (push_queue_.size() >= kMaxQueuedPushUpdates) {
			push_queue_.pop_front();
			dropped = push_dropped_count_.fetch_add(1, std::memory_order_relaxed) + 1;
		}
		push_queue_.push_back(update);
	}
	push_queue_cv_.notify_one();
	if (dropped != 0 && (dropped == 1 || (dropped & (dropped - 1)) == 0)) {
		AppendCoordinatorTrace(plugin_id_,
		                      "push queue dropped_oldest total="
		                      + std::to_string(dropped));
		LogError(ai_, "CoordinatorClient",
		         "PushState queue backpressure; dropped_oldest total="
		         + std::to_string(dropped));
	}
	return dropped == 0;
}

}  // namespace circuit::grpc
