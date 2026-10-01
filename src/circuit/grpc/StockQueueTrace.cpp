// SPDX-License-Identifier: GPL-2.0-only
#include "grpc/StockQueueTrace.h"

#include <bit>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iomanip>
#include <cmath>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace circuit::grpc {
namespace {

constexpr std::size_t kMaximumTraceLineBytes = 32768;
constexpr std::uint64_t kMaximumTraceRecords = 4096;
constexpr std::uint64_t kMaximumTraceBytes = 16 * 1024 * 1024;

class ScopedFd {
public:
	explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}
	~ScopedFd() { if (fd_ >= 0) ::close(fd_); }
	ScopedFd(const ScopedFd&) = delete;
	ScopedFd& operator=(const ScopedFd&) = delete;
	int get() const noexcept { return fd_; }
	int release() noexcept { const int result = fd_; fd_ = -1; return result; }
private:
	int fd_;
};

bool SafeRunId(std::string_view value) {
	if (value.empty() || value.size() > 64) return false;
	for (const unsigned char c : value) {
		if (!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z')
			&& !(c >= '0' && c <= '9') && c != '.' && c != '_' && c != '-') return false;
	}
	return true;
}

bool Utf8(const std::string& value) {
	for (std::size_t i = 0; i < value.size();) {
		const auto first = static_cast<unsigned char>(value[i]);
		if (first == 0) return false;
		if (first < 0x80) { ++i; continue; }
		std::size_t length = 0; std::uint32_t code = 0;
		if ((first & 0xe0) == 0xc0) { length = 2; code = first & 0x1f; }
		else if ((first & 0xf0) == 0xe0) { length = 3; code = first & 0x0f; }
		else if ((first & 0xf8) == 0xf0) { length = 4; code = first & 0x07; }
		else return false;
		if (i + length > value.size()) return false;
		for (std::size_t j = 1; j < length; ++j) {
			const auto next = static_cast<unsigned char>(value[i + j]);
			if ((next & 0xc0) != 0x80) return false;
			code = (code << 6) | (next & 0x3f);
		}
		if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800)
			|| (length == 4 && code < 0x10000) || code > 0x10ffff
			|| (code >= 0xd800 && code <= 0xdfff)) return false;
		i += length;
	}
	return true;
}

bool BoundedText(const std::string& value) {
	if (value.empty() || value.size() > 256) return false;
	return Utf8(value);
}

std::string Json(const std::string& value) {
	std::ostringstream out;
	out << '"';
	for (const unsigned char c : value) {
		switch (c) {
		case '"': out << "\\\""; break;
		case '\\': out << "\\\\"; break;
		case '\b': out << "\\b"; break;
		case '\f': out << "\\f"; break;
		case '\n': out << "\\n"; break;
		case '\r': out << "\\r"; break;
		case '\t': out << "\\t"; break;
		default:
			if (c < 0x20) out << "\\u00" << std::hex << std::setw(2)
				<< std::setfill('0') << static_cast<unsigned>(c) << std::dec;
			else out << static_cast<char>(c);
		}
	}
	out << '"';
	return out.str();
}

std::string Hex(const std::string& bytes) {
	static constexpr char kHex[] = "0123456789abcdef";
	std::string out;
	out.reserve(bytes.size() * 2);
	for (const unsigned char c : bytes) { out.push_back(kHex[c >> 4]); out.push_back(kHex[c & 15]); }
	return out;
}

std::string FloatBits(float value) {
	std::ostringstream out;
	out << std::hex << std::setw(8) << std::setfill('0') << std::nouppercase
		<< std::bit_cast<std::uint32_t>(value);
	return out.str();
}

std::string Base64(const std::string& bytes) {
	static constexpr char kAlphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	for (std::size_t i = 0; i < bytes.size(); i += 3) {
		const std::uint32_t a = static_cast<unsigned char>(bytes[i]);
		const std::uint32_t b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
		const std::uint32_t c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
		const auto value = (a << 16) | (b << 8) | c;
		out.push_back(kAlphabet[(value >> 18) & 63]); out.push_back(kAlphabet[(value >> 12) & 63]);
		out.push_back(i + 1 < bytes.size() ? kAlphabet[(value >> 6) & 63] : '=');
		out.push_back(i + 2 < bytes.size() ? kAlphabet[value & 63] : '=');
	}
	return out;
}

const char* Status(StockQueueReadStatus status) {
	return status == StockQueueReadStatus::Complete ? "complete"
		: status == StockQueueReadStatus::Unavailable ? "unavailable" : "malformed";
}

bool SameEntries(const std::vector<StockQueueEntry>& first,
		const std::vector<StockQueueEntry>& second) {
	if (first.size() != second.size()) return false;
	for (std::size_t i = 0; i < first.size(); ++i) {
		if (first[i].command_id != second[i].command_id
			|| first[i].options != second[i].options || first[i].tag != second[i].tag
			|| first[i].params.size() != second[i].params.size()) return false;
		for (std::size_t j = 0; j < first[i].params.size(); ++j) {
			if (std::bit_cast<std::uint32_t>(first[i].params[j])
				!= std::bit_cast<std::uint32_t>(second[i].params[j])) return false;
		}
	}
	return true;
}

bool Valid(const StockQueueTraceRecord& r) {
	const auto& b = r.basis; const auto& c = r.context;
	if (r.perspective_team_id > 255 || b.token().empty() || b.token().size() > 64
		|| b.state_sequence() == 0 || b.match_incarnation().size() != 16
		|| !BoundedText(b.process_incarnation()) || !BoundedText(b.state_channel_incarnation())
		|| b.snapshot_send_monotonic_ns() == 0 || b.effective_cadence_frames() == 0) return false;
	if (c.profile != kStockTacticalProfile || c.revision != kStockTacticalRevision
		|| c.evidence_scheme != QueueEvidenceScheme::StockLuaSupportedFieldsV1
		|| c.catalogue_id.size() != 16 || c.catalogue_revision == 0
		|| c.engine_version != "2025.06.19" || !BoundedText(c.game_name)
		|| !BoundedText(c.game_version) || c.game_content_sha256.size() != 32
		|| c.actor_id > 31999 || c.actor_lifetime == 0
		|| (c.domain != "production" && c.domain != "rally")) return false;
	if (r.read.canonical_request.empty() || r.read.canonical_request.size() > kStockQueueMaximumBytes
		|| r.read.unit_id != static_cast<std::int32_t>(c.actor_id)
		|| StockQueueDomainName(r.read.domain) != c.domain) return false;
	if (r.read.status == StockQueueReadStatus::Complete) {
		if (r.computed_revision == 0 || r.read.canonical_response.empty()
			|| r.read.canonical_response.size() > kStockQueueMaximumBytes
			|| r.read.entries.size() > kStockQueueMaximumEntries) return false;
		std::size_t parameters = 0;
		for (const auto& entry : r.read.entries) {
			if (entry.params.size() > kStockQueueMaximumParametersPerEntry) return false;
			parameters += entry.params.size();
			if (parameters > kStockQueueMaximumParameters) return false;
			for (float value : entry.params) if (!std::isfinite(value)) return false;
		}
		const auto reparsed = ParseStockQueueResponse(r.read.canonical_response,
			r.read.canonical_request, r.read.domain, r.read.unit_id);
		if (reparsed.status != StockQueueReadStatus::Complete
			|| !SameEntries(reparsed.entries, r.read.entries)
			|| ComputeStockQueueRevision(c, r.read.entries) != r.computed_revision) return false;
	} else if (r.computed_revision != 0 || !r.read.entries.empty()) return false;
	else if (r.read.status == StockQueueReadStatus::Unavailable) {
		const auto reparsed = ParseStockQueueResponse(r.read.canonical_response,
			r.read.canonical_request, r.read.domain, r.read.unit_id);
		if (reparsed.status != StockQueueReadStatus::Unavailable) return false;
	} else if (!r.read.canonical_response.empty()) return false;
	if (r.final_read != r.dispatch.has_value()) return false;
	if (r.dispatch && (r.dispatch->broker_session_id.size() != 16
		|| !BoundedText(r.dispatch->command_channel_incarnation)
		|| r.dispatch->batch_sequence == 0 || r.dispatch->correlation_id == 0
		|| r.dispatch->authority_epoch == 0 || r.dispatch->module_sha256.size() != 32
		|| r.dispatch->module_generation == 0 || r.dispatch->expected_queue_revision == 0)) return false;
	return true;
}

std::string Serialize(const std::string& run_id, std::uint64_t sequence,
		const StockQueueTraceRecord& r) {
	if (!Valid(r)) return {};
	const auto& b = r.basis; const auto& c = r.context;
	std::ostringstream out;
	out << "{\"schema\":\"highbar.barc-stock-queue-trace/v1\",\"runId\":" << Json(run_id)
		<< ",\"sequence\":" << Json(std::to_string(sequence)) << ",\"phase\":\""
		<< (r.final_read ? "final-read" : "sample") << "\",\"readFrame\":" << r.read_frame
		<< ",\"perspectiveTeamId\":" << r.perspective_team_id
		<< ",\"nativeBasis\":{\"token\":" << Json(Base64(b.token()))
		<< ",\"stateSequence\":" << Json(std::to_string(b.state_sequence()))
		<< ",\"frame\":" << b.frame() << ",\"matchIncarnation\":" << Json(Base64(b.match_incarnation()))
		<< ",\"processIncarnation\":" << Json(b.process_incarnation())
		<< ",\"stateChannelIncarnation\":" << Json(b.state_channel_incarnation())
		<< ",\"snapshotSendMonotonicNs\":" << Json(std::to_string(b.snapshot_send_monotonic_ns()))
		<< ",\"effectiveCadenceFrames\":" << b.effective_cadence_frames() << "}"
		<< ",\"context\":{\"profile\":" << Json(c.profile) << ",\"tacticalRevision\":" << c.revision
		<< ",\"queueEvidenceScheme\":" << static_cast<std::uint32_t>(c.evidence_scheme)
		<< ",\"actor\":{\"id\":" << Json(std::to_string(c.actor_id)) << ",\"lifetime\":" << Json(std::to_string(c.actor_lifetime)) << "}"
		<< ",\"catalogueId\":" << Json(Base64(c.catalogue_id)) << ",\"catalogueRevision\":" << Json(std::to_string(c.catalogue_revision))
		<< ",\"engineVersion\":" << Json(c.engine_version) << ",\"gameName\":" << Json(c.game_name)
		<< ",\"gameVersion\":" << Json(c.game_version) << ",\"contentSha256\":" << Json(Hex(c.game_content_sha256))
		<< ",\"domain\":" << Json(c.domain) << "}"
		<< ",\"reader\":{\"kind\":\"CallRules\",\"status\":" << Json(Status(r.read.status))
		<< ",\"request\":" << Json(r.read.canonical_request) << ",\"response\":"
		<< (r.read.status == StockQueueReadStatus::Complete ? Json(r.read.canonical_response) : "null") << "}";
	if (r.read.status != StockQueueReadStatus::Complete) out << ",\"queue\":null";
	else {
		out << ",\"queue\":{\"revision\":" << Json(std::to_string(r.computed_revision)) << ",\"entries\":[";
		for (std::size_t i = 0; i < r.read.entries.size(); ++i) {
			if (i) out << ',';
			const auto& e = r.read.entries[i];
			out << "{\"id\":" << e.command_id << ",\"codedOptions\":" << e.options << ",\"tag\":" << e.tag << ",\"float32params\":[";
			for (std::size_t j = 0; j < e.params.size(); ++j) { if (j) out << ','; out << Json(FloatBits(e.params[j])); }
			out << "]}";
		}
		out << "]}";
	}
	if (!r.dispatch) out << ",\"dispatch\":null";
	else { const auto& d = *r.dispatch;
		out << ",\"dispatch\":{\"brokerSessionId\":" << Json(Base64(d.broker_session_id))
			<< ",\"commandChannelIncarnation\":" << Json(d.command_channel_incarnation)
			<< ",\"batchSequence\":" << Json(std::to_string(d.batch_sequence))
			<< ",\"correlationId\":" << Json(std::to_string(d.correlation_id))
			<< ",\"commandIndex\":" << d.command_index << ",\"authorityEpoch\":" << Json(std::to_string(d.authority_epoch))
			<< ",\"moduleSha256\":" << Json(Hex(d.module_sha256)) << ",\"moduleGeneration\":" << Json(std::to_string(d.module_generation))
			<< ",\"expectedQueueRevision\":" << Json(std::to_string(d.expected_queue_revision))
			<< ",\"matched\":" << (d.matched ? "true" : "false") << "}"; }
	out << '}'; return out.str();
}

}  // namespace

StockQueueTraceSink::StockQueueTraceSink(int fd, std::string run_id,
		bool owns_fd, WriteFunction writer)
	: fd_(fd), run_id_(std::move(run_id)), owns_fd_(owns_fd), writer_(std::move(writer)) {
	if (!writer_) writer_ = [](int fd, const void* data, std::size_t size) { return ::write(fd, data, size); };
}

StockQueueTraceSink::~StockQueueTraceSink() { if (owns_fd_ && fd_ >= 0) ::close(fd_); }

std::unique_ptr<StockQueueTraceSink> StockQueueTraceSink::CreateForTest(
		int fd, std::string run_id, WriteFunction writer) {
	if (fd < 0 || !SafeRunId(run_id)) return nullptr;
	return std::unique_ptr<StockQueueTraceSink>(new StockQueueTraceSink(fd, std::move(run_id), false, std::move(writer)));
}

std::unique_ptr<StockQueueTraceSink> StockQueueTraceSink::AdoptOwned(
		int fd, std::string run_id, WriteFunction writer) noexcept {
	ScopedFd owned(fd);
	if (fd < 0) return nullptr;
	try {
		if (!SafeRunId(run_id)) return nullptr;
		auto result = std::unique_ptr<StockQueueTraceSink>(
			new StockQueueTraceSink(fd, std::move(run_id), true, std::move(writer)));
		owned.release();
		return result;
	} catch (...) {
		return nullptr;
	}
}

std::unique_ptr<StockQueueTraceSink> StockQueueTraceSink::AdoptOwnedForTest(
		int fd, std::string run_id, WriteFunction writer) {
	return AdoptOwned(fd, std::move(run_id), std::move(writer));
}

std::unique_ptr<StockQueueTraceSink> StockQueueTraceSink::CreateFromEnvironment() {
	try {
		const char* path_value = std::getenv("HIGHBAR_STOCK_QUEUE_TRACE");
		const char* run_value = std::getenv("HIGHBAR_STOCK_QUEUE_TRACE_RUN_ID");
		if (path_value == nullptr && run_value == nullptr) return nullptr;
		if (path_value == nullptr || run_value == nullptr || !SafeRunId(run_value)) return nullptr;
		std::string run_id(run_value);
		const std::string path(path_value); const auto slash = path.find_last_of('/');
		if (path.empty() || path.front() != '/' || slash == std::string::npos
			|| slash == 0 || slash + 1 == path.size()) return nullptr;
		const std::string parent = path.substr(0, slash), name = path.substr(slash + 1);
		if (name == "." || name == "..") return nullptr;
		ScopedFd parent_fd(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
		if (parent_fd.get() < 0) return nullptr;
		struct stat parent_stat{};
		if (::fstat(parent_fd.get(), &parent_stat) != 0 || !S_ISDIR(parent_stat.st_mode)
			|| parent_stat.st_uid != ::geteuid() || (parent_stat.st_mode & 0777) != 0700) return nullptr;
		const int fd = ::openat(parent_fd.get(), name.c_str(),
			O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_APPEND, 0600);
		if (fd < 0) return nullptr;
		ScopedFd file_fd(fd);
		struct stat file_stat{};
		if (::fstat(fd, &file_stat) != 0 || !S_ISREG(file_stat.st_mode) || file_stat.st_nlink != 1
			|| file_stat.st_uid != ::geteuid() || (file_stat.st_mode & 0777) != 0600) return nullptr;
		return AdoptOwned(file_fd.release(), std::move(run_id), {});
	} catch (...) {
		return nullptr;
	}
}

bool StockQueueTraceSink::WriteLine(const std::string& line) {
	std::size_t offset = 0;
	while (offset < line.size()) {
		const auto result = writer_(fd_, line.data() + offset, line.size() - offset);
		if (result < 0 && errno == EINTR) continue;
		if (result <= 0 || static_cast<std::size_t>(result) > line.size() - offset) return false;
		offset += static_cast<std::size_t>(result);
	}
	return true;
}

void StockQueueTraceSink::LatchFailure() noexcept {
	failed_ = true;
	const int descriptor = fd_;
	fd_ = -1;
	if (descriptor >= 0) ::close(descriptor);
}

bool StockQueueTraceSink::Record(const StockQueueTraceRecord& record) noexcept {
	if (failed_) return false;
	try {
		auto line = Serialize(run_id_, records_written_ + 1, record);
		if (line.empty() || line.size() >= kMaximumTraceLineBytes
			|| records_written_ >= kMaximumTraceRecords
			|| line.size() + 1 > kMaximumTraceBytes - bytes_written_) {
			LatchFailure();
			return false;
		}
		line.push_back('\n');
		if (!WriteLine(line)) {
			LatchFailure();
			return false;
		}
		++records_written_;
		bytes_written_ += line.size();
		return true;
	} catch (...) {
		LatchFailure();
		return false;
	}
}

}  // namespace circuit::grpc
