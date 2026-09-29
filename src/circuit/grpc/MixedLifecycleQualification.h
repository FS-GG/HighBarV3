// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "highbar/live_control.pb.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace circuit::grpc {

// Qualification-only one-shot handoff used to prove the final native lifetime
// fence. An empty path leaves the object inert. The binding file is deliberately
// read only after a fully valid native Guard admission, allowing a private
// harness to bind the runtime actor/lifetime without exposing a mutation RPC.
class MixedLifecycleQualification {
public:
	struct Request {
		std::string match_incarnation;
		std::uint32_t actor_id = 0;
		std::uint64_t actor_lifetime = 0;
		std::uint64_t batch_seq = 0;
		std::uint64_t client_command_id = 0;
		std::string nonce;
	};

	enum class Decision { kInactive, kHold, kRelease, kFailure };

	explicit MixedLifecycleQualification(std::string path = {})
		: path_(std::move(path)) {}

	bool Enabled() const { return !path_.empty(); }

	bool PublishAcceptedGuard(const ::highbar::v1::LiveCommandBatch& live) noexcept {
		try {
		if (!Enabled() || live.semantic_action() != ::highbar::v1::LIVE_SEMANTIC_ACTION_GUARD
		    || !live.has_actor() || !live.batch().has_client_command_id()) return false;
		const auto parsed = ReadBinding(path_);
		if (!parsed.has_value()) return false;
		const auto& request = *parsed;
		if (request.match_incarnation != live.binding().match_incarnation()
		    || request.actor_id != live.actor().id()
		    || request.actor_lifetime != live.actor().lifetime()
		    || request.batch_seq != live.batch().batch_seq()
		    || request.client_command_id != live.batch().client_command_id()) return false;
		std::lock_guard<std::mutex> lock(mutex_);
		if (state_ != State::kIdle) return false;
		request_ = request;
		state_ = State::kPublished;
		return true;
		} catch (...) {
			// Qualification input must never destabilize the ordinary reader.
			return false;
		}
	}

	std::optional<Request> BeginEngineFrame(std::uint32_t frame) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (state_ != State::kPublished || !request_.has_value()) return std::nullopt;
		request_frame_ = frame;
		state_ = State::kAwaitingLifecycle;
		return request_;
	}

	bool Matches(std::uint64_t batch_seq, std::uint64_t client_command_id,
	             std::uint32_t actor_id, std::uint64_t actor_lifetime,
	             const std::string& match_incarnation) const {
		std::lock_guard<std::mutex> lock(mutex_);
		return request_.has_value()
		    && (state_ == State::kAwaitingLifecycle || state_ == State::kObserved)
		    && request_->batch_seq == batch_seq
		    && request_->client_command_id == client_command_id
		    && request_->actor_id == actor_id
		    && request_->actor_lifetime == actor_lifetime
		    && request_->match_incarnation == match_incarnation;
	}

	void ObserveDestroyed(std::uint32_t actor_id, std::uint64_t actor_lifetime) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (state_ == State::kAwaitingLifecycle && request_.has_value()
		    && request_->actor_id == actor_id
		    && request_->actor_lifetime == actor_lifetime) state_ = State::kObserved;
	}

	void Fail() {
		std::lock_guard<std::mutex> lock(mutex_);
		if (state_ == State::kAwaitingLifecycle || state_ == State::kPublished)
			state_ = State::kFailed;
	}

	Decision DecisionForFrame(std::uint32_t frame) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (state_ == State::kFailed) return Decision::kFailure;
		if (state_ == State::kObserved) {
			state_ = State::kConsumed;
			return Decision::kRelease;
		}
		if (state_ == State::kAwaitingLifecycle) {
			if (frame > request_frame_) {
				state_ = State::kFailed;
				return Decision::kFailure;
			}
			return Decision::kHold;
		}
		return Decision::kInactive;
	}

	static std::optional<Request> ReadBinding(const std::string& path) {
		if (path.empty() || path.front() != '/') return std::nullopt;
		const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0) return std::nullopt;
		struct stat st {};
		if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)
		    || (st.st_mode & 0777) != 0600 || st.st_uid != ::geteuid()
		    || st.st_size <= 0 || st.st_size > 4096) {
			::close(fd);
			return std::nullopt;
		}
		std::string bytes(static_cast<std::size_t>(st.st_size), '\0');
		std::size_t offset = 0;
		while (offset < bytes.size()) {
			const auto count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
			if (count <= 0) { ::close(fd); return std::nullopt; }
			offset += static_cast<std::size_t>(count);
		}
		::close(fd);

		std::map<std::string, std::string> fields;
		std::size_t start = 0;
		while (start < bytes.size()) {
			const auto end = bytes.find('\n', start);
			const auto line = bytes.substr(start, end == std::string::npos ? std::string::npos : end - start);
			if (line.empty() || line.back() == '\r') return std::nullopt;
			const auto equals = line.find('=');
			if (equals == std::string::npos || equals == 0 || equals + 1 >= line.size()
			    || !fields.emplace(line.substr(0, equals), line.substr(equals + 1)).second)
				return std::nullopt;
			if (end == std::string::npos) break;
			start = end + 1;
		}
		if (fields.size() != 7
		    || fields["schema"] != "highbar.barc.mixed-lifecycle-fault/v1") return std::nullopt;
		Request out;
		if (!DecodeHex(fields["match_incarnation_hex"], &out.match_incarnation)
		    || out.match_incarnation.size() != 16
		    || !ParseUnsigned(fields["actor_id"], &out.actor_id) || out.actor_id > 31999
		    || !ParseUnsigned(fields["actor_lifetime"], &out.actor_lifetime) || out.actor_lifetime == 0
		    || !ParseUnsigned(fields["batch_seq"], &out.batch_seq) || out.batch_seq == 0
		    || !ParseUnsigned(fields["client_command_id"], &out.client_command_id) || out.client_command_id == 0
		    || fields["nonce"].size() != 32 || !IsLowerHex(fields["nonce"])) return std::nullopt;
		out.nonce = fields["nonce"];
		return out;
	}

private:
	enum class State { kIdle, kPublished, kAwaitingLifecycle, kObserved, kFailed, kConsumed };

	template <typename T>
	static bool ParseUnsigned(const std::string& text, T* value) {
		if (text.empty() || value == nullptr) return false;
		const auto result = std::from_chars(text.data(), text.data() + text.size(), *value);
		return result.ec == std::errc{} && result.ptr == text.data() + text.size();
	}

	static bool IsLowerHex(std::string_view text) {
		for (const char c : text)
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
		return true;
	}

	static bool DecodeHex(const std::string& text, std::string* bytes) {
		if (bytes == nullptr || text.size() % 2 != 0 || !IsLowerHex(text)) return false;
		bytes->clear(); bytes->reserve(text.size() / 2);
		for (std::size_t i = 0; i < text.size(); i += 2) {
			auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
			bytes->push_back(static_cast<char>((digit(text[i]) << 4) | digit(text[i + 1])));
		}
		return true;
	}

	std::string path_;
	mutable std::mutex mutex_;
	State state_ = State::kIdle;
	std::optional<Request> request_;
	std::uint32_t request_frame_ = 0;
};

}  // namespace circuit::grpc
