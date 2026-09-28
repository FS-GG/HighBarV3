// SPDX-License-Identifier: GPL-2.0-only
//
// HighBarV3 — TransportEndpoint config (T019).
//
// Parses data/config/grpc.json into the struct HighBarService::Bind
// consumes. See data-model.md §6 for the field-by-field contract and
// research.md §2 for the UDS-vs-TCP decision.

#pragma once

#include <cstdint>
#include <string>

namespace circuit {
class CCircuitAI;
}  // namespace circuit

namespace circuit::grpc {

enum class Transport : std::uint8_t {
	kUds,
	kTcp,
};

// 003-snapshot-arm-coverage — configuration for the periodic snapshot
// scheduler. Embedded in TransportEndpoint since grpc.json is the
// single source of truth for plugin-side gateway config. Validation
// bounds per contracts/snapshot-tick.md §Plugin-side config surface:
// cadence ∈ [1, 1024], max_units ∈ [1, 100000]. Out-of-range values
// transition the gateway to Disabled with cfg_invalid at startup.
struct SnapshotTickSettings {
	std::uint32_t snapshot_cadence_frames = 30;
	std::uint32_t snapshot_max_units      = 1000;
};

struct LiveControlSettings {
	// Producer-advertised native age policy. 2000ms is the existing
	// qualified normal-load snapshot max-gap; tighter settings require their
	// own runtime qualification.
	std::uint32_t max_observation_age_ms = 2000;
	std::uint32_t max_reported_units = 64;
};

struct CommandValidationConfig {
	std::string mode = "compatibility";  // compatibility | warning-only | strict
	bool strict = false;
	bool warning_only = false;
	bool allow_legacy_ai_pause = true;
	bool allow_legacy_ai_cheats = true;
	bool reject_unsupported_arms = false;
	bool reject_order_conflicts = false;
	std::uint32_t max_state_age_frames = 300;
	std::uint32_t max_batch_commands = 64;
};

struct TransportEndpoint {
	Transport transport = Transport::kUds;

	// UDS: filesystem path. `$XDG_RUNTIME_DIR` and `${gameid}` are
	// expanded by ResolveUdsPath. If the resolved path exceeds the
	// Linux sun_path limit (108 bytes), ResolveUdsPath falls back
	// to `/tmp/hb-<short>.sock` (research §2, spec Edge Case).
	std::string uds_path = "$XDG_RUNTIME_DIR/highbar-${gameid}.sock";

	// TCP: `host:port` string. Validated to parse to a loopback
	// address at bind time; non-loopback fails startup with a clear
	// error (data-model §6 validation).
	std::string tcp_bind = "127.0.0.1:50511";

	// Auth token file path. `$writeDir` is expanded against the AI's
	// writable data directory (engine callback
	// CALLBACK_DATADIRS_GET_WRITABLE_DIR).
	std::string ai_token_path = "$writeDir/highbar.token";

	// gRPC max receive size. Late-game snapshots exceed the 4MB
	// default; 32MB is enough headroom for 30+ minute matches
	// (plan §Technical Context, data-model §6).
	std::uint32_t max_recv_mb = 32;

	// Per-subscriber fan-out ring size. data-model §2 says 2048 for
	// StateUpdate resume history; data-model §8 separately fixes the
	// per-SubscriberSlot queue at 8192 (fan-out, not resume). This
	// field is the resume-history ring; the fan-out ring is not
	// configurable from grpc.json.
	std::uint32_t ring_size = 2048;

	// 003-snapshot-arm-coverage — periodic snapshot tick config. See
	// SnapshotTickSettings for defaults + validation.
	SnapshotTickSettings snapshot_tick;
	LiveControlSettings live_control;

	CommandValidationConfig command_validation;
};

// Parse data/config/grpc.json relative to the plugin's data root.
// On missing file returns defaults. On parse error: throws
// std::runtime_error so the ctor-time failure propagates through
// CircuitAI's Init path (fail-closed, FR-003a). Validates:
//   * transport in {uds, tcp}
//   * ring_size >= 256
//   * max_recv_mb > 0
//
// The `ai` parameter is used to resolve the data dir via engine
// callbacks. Pass nullptr from unit tests to skip the file read and
// return defaults.
TransportEndpoint LoadTransportConfig(::circuit::CCircuitAI* ai);

// Expands `$XDG_RUNTIME_DIR` and `${gameid}` in endpoint.uds_path.
// If the resolved path is longer than 108 bytes (Linux sun_path
// limit) emits a warning via the log sink and rewrites uds_path to
// `/tmp/hb-<sha1-prefix>.sock`. Returns the resolved final path.
std::string ResolveUdsPath(TransportEndpoint& endpoint,
                           ::circuit::CCircuitAI* ai);

}  // namespace circuit::grpc
