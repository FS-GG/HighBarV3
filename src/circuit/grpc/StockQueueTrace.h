// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "grpc/StockQueueReader.h"
#include "grpc/TacticalNativeState.h"
#include "highbar/live_control.pb.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>

namespace circuit::grpc {

struct StockQueueDispatchIdentity {
	std::string broker_session_id;
	std::string command_channel_incarnation;
	std::uint64_t batch_sequence = 0;
	std::uint64_t correlation_id = 0;
	std::uint32_t command_index = 0;
	std::uint64_t authority_epoch = 0;
	std::string module_sha256;
	std::uint64_t module_generation = 0;
	std::uint64_t expected_queue_revision = 0;
	bool matched = false;
};

struct StockQueueTraceRecord {
	bool final_read = false;
	std::uint32_t read_frame = 0;
	std::uint32_t perspective_team_id = 0;
	::highbar::v1::NativeObservationBasis basis;
	StockQueueRevisionContext context;
	StockQueueReadResult read;
	std::uint64_t computed_revision = 0;
	std::optional<StockQueueDispatchIdentity> dispatch;
};

class StockQueueTraceSink {
public:
	using WriteFunction = std::function<ssize_t(int, const void*, std::size_t)>;
	~StockQueueTraceSink();
	StockQueueTraceSink(const StockQueueTraceSink&) = delete;
	StockQueueTraceSink& operator=(const StockQueueTraceSink&) = delete;

	static std::unique_ptr<StockQueueTraceSink> CreateFromEnvironment();
	static std::unique_ptr<StockQueueTraceSink> CreateForTest(
		int fd, std::string run_id, WriteFunction writer = {});

	bool Record(const StockQueueTraceRecord& record);
	bool failed() const { return failed_; }
	std::uint64_t records_written() const { return records_written_; }
	std::uint64_t bytes_written() const { return bytes_written_; }

private:
	StockQueueTraceSink(int fd, std::string run_id, bool owns_fd,
		WriteFunction writer);
	bool WriteLine(const std::string& line);

	int fd_ = -1;
	std::string run_id_;
	bool owns_fd_ = false;
	bool failed_ = false;
	std::uint64_t records_written_ = 0;
	std::uint64_t bytes_written_ = 0;
	WriteFunction writer_;
};

}  // namespace circuit::grpc
