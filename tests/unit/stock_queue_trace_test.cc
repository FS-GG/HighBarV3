// SPDX-License-Identifier: GPL-2.0-only
#include "grpc/StockQueueTrace.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
thread_local bool fail_next_allocation = false;
}

void* operator new(std::size_t size) {
	if (fail_next_allocation) {
		fail_next_allocation = false;
		throw std::bad_alloc();
	}
	if (void* value = std::malloc(size)) return value;
	throw std::bad_alloc();
}

void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }

namespace circuit::grpc {
namespace {

std::string Response(StockQueueDomain domain, std::int32_t unit, bool unavailable) {
	const auto request = BuildStockQueueRequest(domain, unit);
	std::string response = "BARC_QUEUE_RESPONSE/1\nlength=00000000\nbridge=barc-stock-queue-reader-v1\nrequest-sha256="
		+ Sha256Hex(request) + "\nstatus=" + (unavailable ? "unavailable" : "ok")
		+ "\ndomain=" + StockQueueDomainName(domain) + "\nunit=" + std::to_string(unit) + "\n";
	if (unavailable) response += "reason=wrong-team\ncount=0\n";
	else response += "count=1\nrow=" + std::string(StockQueueDomainName(domain)) + "|-1|32|9|3f800000,80000000\n";
	response += "end\n";
	const auto size = std::to_string(response.size());
	response.replace(response.find("length=") + 7, 8, std::string(8 - size.size(), '0') + size);
	return response;
}

StockQueueTraceRecord Record(bool final_read = false) {
	StockQueueTraceRecord value;
	value.final_read = final_read; value.read_frame = 41; value.perspective_team_id = 3;
	value.basis.set_token("token"); value.basis.set_state_sequence(12); value.basis.set_frame(40);
	value.basis.set_match_incarnation(std::string(16, 'm'));
	value.basis.set_process_incarnation("process"); value.basis.set_state_channel_incarnation("state");
	value.basis.set_snapshot_send_monotonic_ns(99); value.basis.set_effective_cadence_frames(3);
	value.context.profile = kStockTacticalProfile; value.context.revision = kStockTacticalRevision;
	value.context.evidence_scheme = QueueEvidenceScheme::StockLuaSupportedFieldsV1;
	value.context.catalogue_id = std::string(16, 'c'); value.context.catalogue_revision = 8;
	value.context.engine_version = "2026.07.04"; value.context.game_name = "BAR";
	value.context.game_version = "test"; value.context.game_content_sha256 = std::string(32, 'h');
	value.context.actor_id = 0; value.context.actor_lifetime = 7; value.context.domain = "production";
	value.read.status = StockQueueReadStatus::Complete; value.read.domain = StockQueueDomain::Production;
	value.read.unit_id = 0; value.read.canonical_request = BuildStockQueueRequest(StockQueueDomain::Production, 0);
	value.read.canonical_response = Response(StockQueueDomain::Production, 0, false);
	value.read.entries = {{-1, 32, 9, {1.0f, -0.0f}}};
	value.computed_revision = ComputeStockQueueRevision(value.context, value.read.entries);
	if (final_read) {
		StockQueueDispatchIdentity dispatch; dispatch.broker_session_id = std::string(16, 's');
		dispatch.command_channel_incarnation = "channel"; dispatch.batch_sequence = 2;
		dispatch.correlation_id = 3; dispatch.command_index = 0; dispatch.authority_epoch = 4;
		dispatch.module_sha256 = std::string(32, 'z'); dispatch.module_generation = 5;
		dispatch.expected_queue_revision = value.computed_revision; dispatch.matched = true;
		value.dispatch = dispatch;
	}
	return value;
}

bool ProcessHasOpenDescriptorFor(const std::filesystem::path& path) {
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd", error)) {
		const auto target = std::filesystem::read_symlink(entry.path(), error);
		if (!error && target == path) return true;
		error.clear();
	}
	return false;
}

TEST(StockQueueTrace, WritesClosedSampleAndFinalIdentityWithSignedZeroBits) {
	char path[] = "/tmp/highbar-stock-trace-test-XXXXXX"; const int fd = ::mkstemp(path);
	ASSERT_GE(fd, 0); auto sink = StockQueueTraceSink::CreateForTest(fd, "run-1"); ASSERT_TRUE(sink);
	EXPECT_TRUE(sink->Record(Record())); EXPECT_TRUE(sink->Record(Record(true)));
	::lseek(fd, 0, SEEK_SET); std::string bytes; char buffer[4096]; ssize_t count;
	while ((count = ::read(fd, buffer, sizeof buffer)) > 0) bytes.append(buffer, count);
	EXPECT_NE(bytes.find("\"sequence\":\"1\",\"phase\":\"sample\""), std::string::npos);
	EXPECT_NE(bytes.find("\"float32params\":[\"3f800000\",\"80000000\"]"), std::string::npos);
	EXPECT_NE(bytes.find("\"sequence\":\"2\",\"phase\":\"final-read\""), std::string::npos);
	EXPECT_NE(bytes.find("\"brokerSessionId\":\"c3Nzc3Nzc3Nzc3Nzc3Nzcw==\""), std::string::npos);
	::close(fd); ::unlink(path);
}

TEST(StockQueueTrace, RefusesHistoricalEngineIdentity) {
	char path[] = "/tmp/highbar-stock-engine-test-XXXXXX";
	const int fd = ::mkstemp(path); ASSERT_GE(fd, 0);
	auto sink = StockQueueTraceSink::CreateForTest(fd, "run"); ASSERT_TRUE(sink);
	auto record = Record(); record.context.engine_version = "2025.06.19";
	record.computed_revision = ComputeStockQueueRevision(record.context, record.read.entries);
	EXPECT_FALSE(sink->Record(record)); EXPECT_EQ(sink->records_written(), 0u);
	sink.reset(); ::unlink(path);
}

TEST(StockQueueTrace, PartialWritesCompleteButZeroWriteLatchesFailure) {
	char path[] = "/tmp/highbar-stock-trace-write-XXXXXX"; const int fd = ::mkstemp(path); ASSERT_GE(fd, 0);
	auto partial = StockQueueTraceSink::CreateForTest(fd, "run", [](int descriptor, const void* data, std::size_t size) {
		return ::write(descriptor, data, std::min<std::size_t>(size, 7));
	});
	EXPECT_TRUE(partial->Record(Record())); EXPECT_FALSE(partial->failed()); partial.reset();
	auto stopped = StockQueueTraceSink::CreateForTest(fd, "run", [](int, const void*, std::size_t) { return ssize_t{0}; });
	EXPECT_FALSE(stopped->Record(Record())); EXPECT_TRUE(stopped->failed()); EXPECT_FALSE(stopped->Record(Record()));
	EXPECT_EQ(::fcntl(fd, F_GETFD), -1); EXPECT_EQ(errno, EBADF);
	::close(fd); ::unlink(path);
}

TEST(StockQueueTrace, AllocationFailureClosesDescriptorAndPermanentlyRefuses) {
	char path[] = "/tmp/highbar-stock-trace-alloc-XXXXXX"; const int fd = ::mkstemp(path); ASSERT_GE(fd, 0);
	auto sink = StockQueueTraceSink::CreateForTest(fd, "run"); ASSERT_TRUE(sink);
	const auto record = Record();
	fail_next_allocation = true;
	EXPECT_FALSE(sink->Record(record));
	EXPECT_TRUE(sink->failed());
	EXPECT_EQ(::fcntl(fd, F_GETFD), -1); EXPECT_EQ(errno, EBADF);
	EXPECT_FALSE(sink->Record(record));
	EXPECT_EQ(sink->records_written(), 0u); EXPECT_EQ(sink->bytes_written(), 0u);
	::unlink(path);
}

TEST(StockQueueTrace, ThrowingWriterCannotEscapeOrChangeCallerControlFlow) {
	char path[] = "/tmp/highbar-stock-trace-throw-XXXXXX"; const int fd = ::mkstemp(path); ASSERT_GE(fd, 0);
	auto sink = StockQueueTraceSink::CreateForTest(fd, "run",
		[](int, const void*, std::size_t) -> ssize_t { throw std::runtime_error("write failed"); });
	ASSERT_TRUE(sink);
	bool synchronized_effect_reached = false;
	const auto dispatch = [&] {
		(void)sink->Record(Record(true));
		synchronized_effect_reached = true;
		return true;
	};
	EXPECT_TRUE(dispatch());
	EXPECT_TRUE(synchronized_effect_reached);
	EXPECT_TRUE(sink->failed());
	EXPECT_EQ(::fcntl(fd, F_GETFD), -1); EXPECT_EQ(errno, EBADF);
	EXPECT_FALSE(sink->Record(Record(true)));
	::unlink(path);
}

TEST(StockQueueTrace, OwnedDescriptorClosesWhenSinkConstructionAllocationFails) {
	char path[] = "/tmp/highbar-stock-trace-adopt-XXXXXX"; const int fd = ::mkstemp(path); ASSERT_GE(fd, 0);
	std::string run = "run";
	fail_next_allocation = true;
	auto sink = StockQueueTraceSink::AdoptOwnedForTest(fd, std::move(run));
	EXPECT_FALSE(sink);
	EXPECT_EQ(::fcntl(fd, F_GETFD), -1); EXPECT_EQ(errno, EBADF);
	::unlink(path);
}

TEST(StockQueueTrace, MissingBasisMalformedShapeAndOversizeFailClosed) {
	char path[] = "/tmp/highbar-stock-trace-invalid-XXXXXX"; const int fd = ::mkstemp(path); ASSERT_GE(fd, 0);
	auto missing = Record(); missing.basis.clear_token();
	auto sink = StockQueueTraceSink::CreateForTest(fd, "run"); ASSERT_TRUE(sink);
	EXPECT_FALSE(sink->Record(missing)); EXPECT_TRUE(sink->failed());
	EXPECT_EQ(::fcntl(fd, F_GETFD), -1); EXPECT_EQ(errno, EBADF);
	::close(fd); ::unlink(path);
}

TEST(StockQueueTrace, UnavailableAndMalformedNeverRetainResponseOrMintQueue) {
	char path[] = "/tmp/highbar-stock-trace-status-XXXXXX"; const int fd = ::mkstemp(path); ASSERT_GE(fd, 0);
	auto sink = StockQueueTraceSink::CreateForTest(fd, "run"); ASSERT_TRUE(sink);
	auto unavailable = Record(); unavailable.read.status = StockQueueReadStatus::Unavailable;
	unavailable.read.canonical_response = Response(StockQueueDomain::Production, 0, true);
	unavailable.read.entries.clear(); unavailable.computed_revision = 0;
	EXPECT_TRUE(sink->Record(unavailable));
	auto malformed = unavailable; malformed.read.status = StockQueueReadStatus::Malformed;
	malformed.read.canonical_response.clear();
	EXPECT_TRUE(sink->Record(malformed));
	::lseek(fd, 0, SEEK_SET); std::string bytes; char buffer[4096]; ssize_t count;
	while ((count = ::read(fd, buffer, sizeof buffer)) > 0) bytes.append(buffer, count);
	EXPECT_EQ(bytes.find("reason=wrong-team"), std::string::npos);
	EXPECT_NE(bytes.find("\"status\":\"unavailable\",\"request\":"), std::string::npos);
	EXPECT_NE(bytes.find("\"status\":\"malformed\",\"request\":"), std::string::npos);
	EXPECT_NE(bytes.find("\"response\":null},\"queue\":null"), std::string::npos);
	::close(fd); ::unlink(path);
}

TEST(StockQueueTrace, RecordCeilingLatchesWithoutWritingPastBound) {
	const int fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC); ASSERT_GE(fd, 0);
	auto sink = StockQueueTraceSink::CreateForTest(fd, "run",
		[](int, const void*, std::size_t size) { return static_cast<ssize_t>(size); });
	ASSERT_TRUE(sink);
	for (int index = 0; index < 4096; ++index) ASSERT_TRUE(sink->Record(Record())) << index;
	EXPECT_FALSE(sink->Record(Record())); EXPECT_TRUE(sink->failed()); EXPECT_EQ(sink->records_written(), 4096u);
	EXPECT_EQ(::fcntl(fd, F_GETFD), -1); EXPECT_EQ(errno, EBADF);
}

TEST(StockQueueTrace, EnvironmentRequiresOwnedModeAndCreateExclusive) {
	::unsetenv("HIGHBAR_STOCK_QUEUE_TRACE");
	::unsetenv("HIGHBAR_STOCK_QUEUE_TRACE_RUN_ID");
	EXPECT_FALSE(StockQueueTraceSink::CreateFromEnvironment());
	const auto root = std::filesystem::temp_directory_path() / ("highbar-stock-trace-parent-" + std::to_string(::getpid()));
	std::filesystem::create_directory(root); ::chmod(root.c_str(), 0755);
	const auto path = root / "trace.jsonl";
	::setenv("HIGHBAR_STOCK_QUEUE_TRACE", path.c_str(), 1); ::setenv("HIGHBAR_STOCK_QUEUE_TRACE_RUN_ID", "native.run", 1);
	EXPECT_FALSE(StockQueueTraceSink::CreateFromEnvironment());
	::chmod(root.c_str(), 0700); auto sink = StockQueueTraceSink::CreateFromEnvironment(); ASSERT_TRUE(sink);
	struct stat info{}; ASSERT_EQ(::stat(path.c_str(), &info), 0); EXPECT_EQ(info.st_mode & 0777, 0600); EXPECT_EQ(info.st_nlink, 1);
	EXPECT_TRUE(ProcessHasOpenDescriptorFor(path));
	EXPECT_FALSE(StockQueueTraceSink::CreateFromEnvironment());
	sink.reset(); EXPECT_FALSE(ProcessHasOpenDescriptorFor(path));
	std::filesystem::remove_all(root); ::unsetenv("HIGHBAR_STOCK_QUEUE_TRACE"); ::unsetenv("HIGHBAR_STOCK_QUEUE_TRACE_RUN_ID");
}

}  // namespace
}  // namespace circuit::grpc
