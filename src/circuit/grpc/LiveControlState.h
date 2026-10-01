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

enum class BasisLookupResult {
	kKnown,
	kSequenceAbsent,
	kValueMismatch,
};

// Shared background/engine-thread state for the opt-in live profile. Network
// threads may change authority; engine callbacks own identities and snapshot
// bases. All compound transitions are linearized by one mutex.
class LiveControlState {
public:
	using Clock = std::chrono::steady_clock;
	class LockedDispatchState {
	public:
		LockedDispatchState(const LockedDispatchState&) = delete;
		LockedDispatchState& operator=(const LockedDispatchState&) = delete;
		std::uint64_t OwnedLifetime(std::uint32_t id) const;

	private:
		friend class LiveControlState;
		explicit LockedDispatchState(const LiveControlState* state)
			: state_(state) {}
		const LiveControlState* state_;
	};
	static std::string NewMatchIncarnation();

	LiveControlState(std::string plugin_id,
	                 std::string process_incarnation,
	                 std::string match_incarnation,
	                 std::string state_channel_incarnation,
	                 std::string command_channel_incarnation,
	                 std::string control_channel_incarnation,
	                 std::size_t max_reported_units = 64);

	const std::string& PluginId() const { return plugin_id_; }
	const std::string& ProcessIncarnation() const { return process_incarnation_; }
	const std::string& MatchIncarnation() const { return match_incarnation_; }
	const std::string& StateChannelIncarnation() const { return state_channel_incarnation_; }
	std::string CommandChannelIncarnation() const;
	std::string ControlChannelIncarnation() const;
	void ReplaceChannels(std::string command_channel_incarnation,
	                     std::string control_channel_incarnation);

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
	LiveFenceResult DispatchGuardedWithLockedState(
		const QueuedCommand& command,
		const std::function<bool(const LockedDispatchState&)>& dispatch,
		Clock::time_point now = Clock::now()) const;
	bool LegacyGameplayAllowed() const;
	bool DispatchLegacyGuarded(const std::function<bool()>& dispatch) const;

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
		std::uint32_t effective_cadence_frames,
		std::chrono::milliseconds maximum_age,
		Clock::time_point emitted_at = Clock::now());
	BasisLookupResult ClassifyBasis(
		const ::highbar::v1::NativeObservationBasis& basis) const;
	bool BasisKnown(const ::highbar::v1::NativeObservationBasis& basis) const;
	std::optional<Clock::time_point> BasisExpiry(
		const ::highbar::v1::NativeObservationBasis& basis) const;
	std::optional<std::vector<::highbar::v1::NativeLiveUnitMetadata>>
	SnapshotUnitMetadata();
	bool LiveBatchFresh(const ::highbar::v1::LiveCommandBatch& batch) const;
	void RememberLiveBatch(const ::highbar::v1::LiveCommandBatch& batch);
	void RecordTacticalCatalogue(const std::string& catalogue_id,
	                            std::uint64_t revision, bool complete);
	void RecordTacticalSnapshot(
		const ::highbar::v1::TacticalSnapshotMetadata& snapshot);
	LiveFenceResult CheckTacticalCommand(
		const ::highbar::v1::LiveCommandBatch& batch) const;

private:
	struct UnitLife { std::uint64_t lifetime = 0; bool present = false; bool visual = false; };
	struct BasisEntry {
		::highbar::v1::NativeObservationBasis basis;
		Clock::time_point emitted_at;
		Clock::time_point expires_at;
	};

	bool BindingMatchesLocal(const ::highbar::v1::LiveBinding& binding) const;
	bool BindingEquals(const ::highbar::v1::LiveBinding& a,
	                  const ::highbar::v1::LiveBinding& b) const;
	static std::string BasisToken(std::uint64_t seq, std::uint64_t send_ns);
	LiveFenceResult CheckQueuedCommandLocked(
		const QueuedCommand& command, Clock::time_point now) const;
	LiveFenceResult CheckTacticalCommandLocked(
		const ::highbar::v1::LiveCommandBatch& batch) const;
	BasisLookupResult ClassifyBasisLocked(
		const ::highbar::v1::NativeObservationBasis& basis) const;
	std::uint64_t OwnedLifetimeLocked(std::uint32_t id) const;

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
	bool live_session_engaged_ = false;
	Clock::time_point lease_deadline_{};
	std::size_t max_reported_units_ = 64;
	std::unordered_map<std::uint32_t, UnitLife> owned_;
	std::unordered_map<std::uint32_t, UnitLife> enemies_;
	std::unordered_map<std::uint64_t, BasisEntry> bases_;
	std::unordered_map<std::string, std::uint64_t> admitted_live_batches_;
	std::string tactical_catalogue_id_;
	std::uint64_t tactical_catalogue_revision_ = 0;
	bool tactical_catalogue_complete_ = false;
	std::optional<::highbar::v1::TacticalSnapshotMetadata> tactical_snapshot_;
	static constexpr std::size_t kMaxBases = 256;
};

}  // namespace circuit::grpc
