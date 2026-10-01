// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace circuit::grpc {

inline constexpr const char* kStockQueueBridgeId =
	"barc-stock-queue-reader-v1";
inline constexpr std::size_t kStockQueueMaximumBytes = 8192;
inline constexpr std::size_t kStockQueueMaximumEntries = 64;
inline constexpr std::size_t kStockQueueMaximumParametersPerEntry = 16;
inline constexpr std::size_t kStockQueueMaximumParameters = 256;
inline constexpr std::size_t kStockQueueMaximumLineBytes = 512;

enum class StockQueueDomain {
	Production,
	Rally,
};

struct StockQueueEntry {
	std::int32_t command_id = 0;
	std::uint16_t options = 0;
	std::int32_t tag = 0;
	std::vector<float> params;
};

enum class StockQueueReadStatus {
	Complete,
	Unavailable,
	Malformed,
};

struct StockQueueReadResult {
	StockQueueReadStatus status = StockQueueReadStatus::Malformed;
	StockQueueDomain domain = StockQueueDomain::Production;
	std::int32_t unit_id = -1;
	std::string unavailable_reason;
	std::string canonical_response;
	std::vector<StockQueueEntry> entries;
};

using StockQueueCallRules =
	std::function<std::string(const char*, std::size_t)>;

const char* StockQueueDomainName(StockQueueDomain domain);
std::string BuildStockQueueRequest(StockQueueDomain domain,
	std::int32_t unit_id);
std::array<std::uint8_t, 32> Sha256Digest(const std::string& value);
std::string Sha256Hex(const std::string& value);

StockQueueReadResult ParseStockQueueResponse(
	const std::string& response,
	const std::string& exact_request,
	StockQueueDomain expected_domain,
	std::int32_t expected_unit_id);

// This is the one bridge reader used for both observation and the final
// post-control-acquisition fence. Callers supply the actual LuaRules callback;
// no retry, paging, fallback or mutable cache is hidden here.
StockQueueReadResult ReadStockQueue(
	StockQueueDomain domain,
	std::int32_t unit_id,
	const StockQueueCallRules& call_rules);

}  // namespace circuit::grpc
