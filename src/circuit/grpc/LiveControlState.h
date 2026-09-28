// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "highbar/live_control.pb.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace circuit::grpc {
struct QueuedCommand;

struct LiveFenceResult {
	bool ok = false;
	::highbar::v1::LiveFenceReason reason =
		::highbar::v1::LIVE_FENCE_AUTHORITY_NOT_CONFIRMED;
};

// Shared background/engine-thread state for the opt-in live profile. Network
// threads may change authority; engine callbacks own identities and snapshot
// bases. All compound transitions are linearized by one mutex.
class LiveControlState {
public:
	using Clock = std::chrono::steady_clock;

	LiveControlState(std::string plugin_id,
	                 std::string process_incarnation,
	                 std::string match_incarnation,
	                 std::string state_channel_incarnation,
	                 std::string command_channel_incarnation,
	                 std::string control_channel_incarnation);

	const std::string& PluginId() const { return plugin_id_; }
	const std::string& ProcessIncarnation() const { return process_incarnation_; }
	const std::string& MatchIncarnation() const { return match_incarnation_; }
	const std::string& StateChannelIncarnation() const { return state_channel_incarnation_; }
	const std::string& CommandChannelIncarnation() const { return command_channel_incarnation_; }
	const std::string& ControlChannelIncarnation() const { return control_channel_incarnation_; }

	::highbar::v1::LiveControlAckReport ApplyDirective(
		const ::highbar::v1::LiveControlDirective& directive,
		Clock::time_point now = Clock::now());
	LiveFenceResult CheckAuthority(
		const ::highbar::v1::LiveBinding& binding,
		Clock::time_point now = Clock::now()) const;
	LiveFenceResult CheckQueuedCommand(
		const QueuedCommand& command,
		Clock::time_point now = Clock::now()) const;
	LiveFenceResult DispatchGuarded(
		const QueuedCommand& command,
		const std::function<bool()>& dispatch,
		Clock::time_point now = Clock::now()) const;

	std::uint64_t MarkOwnedPresent(std::uint32_t id);
	void MarkOwnedRemoved(std::uint32_t id);
	std::uint64_t MarkEnemyPresent(std::uint32_t id, bool visual);
	void MarkEnemyVisual(std::uint32_t id, bool visual);
	void MarkEnemyRemoved(std::uint32_t id);
	std::uint64_t OwnedLifetime(std::uint32_t id) const;
	std::uint64_t EnemyLifetime(std::uint32_t id) const;
	bool EnemyVisual(std::uint32_t id) const;

	::highbar::v1::NativeObservationBasis RecordBasis(
		std::uint64_t state_sequence, std::uint32_t frame,
		std::uint64_t snapshot_send_monotonic_ns,
		std::uint32_t effective_cadence_frames);
	bool BasisKnown(const ::highbar::v1::NativeObservationBasis& basis) const;
	std::vector<::highbar::v1::NativeLiveUnitMetadata> SnapshotUnitMetadata(
		std::size_t maximum) const;

private:
	struct UnitLife { std::uint64_t lifetime = 0; bool present = false; bool visual = false; };
	struct BasisEntry { ::highbar::v1::NativeObservationBasis basis; };

	bool BindingMatchesLocal(const ::highbar::v1::LiveBinding& binding) const;
	bool BindingEquals(const ::highbar::v1::LiveBinding& a,
	                  const ::highbar::v1::LiveBinding& b) const;
	static std::string BasisToken(std::uint64_t seq, std::uint64_t send_ns);
	LiveFenceResult CheckQueuedCommandLocked(
		const QueuedCommand& command, Clock::time_point now) const;

	std::string plugin_id_;
	std::string process_incarnation_;
	std::string match_incarnation_;
	std::string state_channel_incarnation_;
	std::string command_channel_incarnation_;
	std::string control_channel_incarnation_;
	mutable std::mutex mutex_;
	std::optional<::highbar::v1::LiveBinding> binding_;
	std::uint64_t control_sequence_ = 0;
	bool revoked_ = true;
	Clock::time_point lease_deadline_{};
	std::unordered_map<std::uint32_t, UnitLife> owned_;
	std::unordered_map<std::uint32_t, UnitLife> enemies_;
	std::unordered_map<std::uint64_t, BasisEntry> bases_;
	static constexpr std::size_t kMaxBases = 256;
};

}  // namespace circuit::grpc
