// SPDX-License-Identifier: GPL-2.0-only
#include "grpc/StockQueueReader.h"

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>

namespace circuit::grpc {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256Constants = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

std::array<std::uint8_t, 32> ComputeSha256(const std::string& input) {
	auto rotate_right = [](std::uint32_t value, int count) {
		return (value >> count) | (value << (32 - count));
	};
	std::vector<std::uint8_t> data(input.begin(), input.end());
	const std::uint64_t bit_count = static_cast<std::uint64_t>(data.size()) * 8;
	data.push_back(0x80);
	while ((data.size() % 64) != 56) data.push_back(0);
	for (int index = 7; index >= 0; --index) {
		data.push_back(static_cast<std::uint8_t>(bit_count >> (index * 8)));
	}
	std::uint32_t hash[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
		0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	for (std::size_t offset = 0; offset < data.size(); offset += 64) {
		std::uint32_t words[64]{};
		for (int index = 0; index < 16; ++index) {
			words[index] = (static_cast<std::uint32_t>(data[offset + index * 4]) << 24)
				| (static_cast<std::uint32_t>(data[offset + index * 4 + 1]) << 16)
				| (static_cast<std::uint32_t>(data[offset + index * 4 + 2]) << 8)
				| data[offset + index * 4 + 3];
		}
		for (int index = 16; index < 64; ++index) {
			const auto s0 = rotate_right(words[index - 15], 7)
				^ rotate_right(words[index - 15], 18) ^ (words[index - 15] >> 3);
			const auto s1 = rotate_right(words[index - 2], 17)
				^ rotate_right(words[index - 2], 19) ^ (words[index - 2] >> 10);
			words[index] = words[index - 16] + s0 + words[index - 7] + s1;
		}
		std::uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3];
		std::uint32_t e = hash[4], f = hash[5], g = hash[6], h = hash[7];
		for (int index = 0; index < 64; ++index) {
			const auto s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
			const auto choose = (e & f) ^ ((~e) & g);
			const auto first = h + s1 + choose + kSha256Constants[index] + words[index];
			const auto s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
			const auto majority = (a & b) ^ (a & c) ^ (b & c);
			const auto second = s0 + majority;
			h = g; g = f; f = e; e = d + first;
			d = c; c = b; b = a; a = first + second;
		}
		hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d;
		hash[4] += e; hash[5] += f; hash[6] += g; hash[7] += h;
	}
	std::array<std::uint8_t, 32> output{};
	for (int word = 0; word < 8; ++word) {
		for (int byte = 0; byte < 4; ++byte) {
			output[word * 4 + byte] =
				static_cast<std::uint8_t>(hash[word] >> (24 - byte * 8));
		}
	}
	return output;
}

bool SevenBitAsciiWithoutNul(const std::string& value) {
	for (unsigned char byte : value) if (byte == 0 || byte > 0x7f) return false;
	return true;
}

template <typename T>
bool ParseInteger(std::string_view value, T* output) {
	if (value.empty()) return false;
	T parsed{};
	const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
	if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return false;
	*output = parsed;
	return true;
}

bool SplitExact(std::string_view value, char separator,
		std::vector<std::string_view>* output) {
	output->clear();
	std::size_t start = 0;
	while (start <= value.size()) {
		const auto end = value.find(separator, start);
		output->push_back(value.substr(start, end == std::string_view::npos
			? value.size() - start : end - start));
		if (end == std::string_view::npos) break;
		start = end + 1;
	}
	return true;
}

bool ParseFloatBits(std::string_view value, float* output) {
	if (value.size() != 8) return false;
	std::uint32_t bits = 0;
	for (char character : value) {
		bits <<= 4;
		if (character >= '0' && character <= '9') bits |= character - '0';
		else if (character >= 'a' && character <= 'f') bits |= character - 'a' + 10;
		else return false;
	}
	const float parsed = std::bit_cast<float>(bits);
	if (!std::isfinite(parsed)) return false;
	*output = parsed;
	return true;
}

std::vector<std::string_view> Lines(const std::string& value) {
	std::vector<std::string_view> result;
	std::size_t start = 0;
	while (start < value.size()) {
		const auto end = value.find('\n', start);
		if (end == std::string::npos) return {};
		result.emplace_back(value.data() + start, end - start);
		start = end + 1;
	}
	return result;
}

bool Field(std::string_view line, std::string_view name, std::string_view* value) {
	if (line.size() <= name.size() + 1 || line.substr(0, name.size()) != name
		|| line[name.size()] != '=') return false;
	*value = line.substr(name.size() + 1);
	return true;
}

}  // namespace

const char* StockQueueDomainName(StockQueueDomain domain) {
	return domain == StockQueueDomain::Production ? "production" : "rally";
}

std::string Sha256Hex(const std::string& value) {
	static constexpr char kHex[] = "0123456789abcdef";
	const auto bytes = Sha256Digest(value);
	std::string output;
	output.reserve(64);
	for (auto byte : bytes) {
		output.push_back(kHex[byte >> 4]);
		output.push_back(kHex[byte & 15]);
	}
	return output;
}

std::array<std::uint8_t, 32> Sha256Digest(const std::string& value) {
	return ComputeSha256(value);
}

std::string BuildStockQueueRequest(StockQueueDomain domain,
		std::int32_t unit_id) {
	if (unit_id < 0) return {};
	std::string request = "BARC_QUEUE_REQUEST/1\nlength=00000000\nbridge=";
	request += kStockQueueBridgeId;
	request += "\ndomain=";
	request += StockQueueDomainName(domain);
	request += "\nunit=" + std::to_string(unit_id) + "\nend\n";
	if (request.size() > kStockQueueMaximumBytes || request.size() > 99999999) {
		return {};
	}
	const auto length = std::to_string(request.size());
	request.replace(28 + 8 - length.size(), length.size(), length);
	return request;
}

StockQueueReadResult ParseStockQueueResponse(const std::string& response,
		const std::string& exact_request, StockQueueDomain expected_domain,
		std::int32_t expected_unit_id) {
	StockQueueReadResult result;
	result.domain = expected_domain;
	result.unit_id = expected_unit_id;
	result.canonical_request = exact_request;
	if (response.empty() || response.size() > kStockQueueMaximumBytes
		|| !SevenBitAsciiWithoutNul(response) || response.back() != '\n') return result;
	const auto lines = Lines(response);
	if (lines.size() < 8 || lines[0] != "BARC_QUEUE_RESPONSE/1"
		|| lines.back() != "end") return result;
	for (auto line : lines) {
		if (line.size() > kStockQueueMaximumLineBytes) return result;
	}
	std::string_view field;
	if (!Field(lines[1], "length", &field) || field.size() != 8) return result;
	std::size_t declared = 0;
	if (!ParseInteger(field, &declared) || declared != response.size()) return result;
	if (!Field(lines[2], "bridge", &field) || field != kStockQueueBridgeId) return result;
	if (!Field(lines[3], "request-sha256", &field)
		|| field != Sha256Hex(exact_request)) return result;
	std::string_view status;
	if (!Field(lines[4], "status", &status)
		|| (status != "ok" && status != "unavailable")) return result;
	if (!Field(lines[5], "domain", &field)
		|| field != StockQueueDomainName(expected_domain)) return result;
	std::int32_t unit = -1;
	if (!Field(lines[6], "unit", &field) || !ParseInteger(field, &unit)
		|| unit != expected_unit_id) return result;
	std::size_t cursor = 7;
	if (status == "unavailable") {
		if (lines.size() != 10 || !Field(lines[cursor++], "reason", &field)
			|| field.empty() || field.size() > 64) return result;
		result.unavailable_reason = std::string(field);
	}
	std::size_t count = 0;
	if (!Field(lines[cursor++], "count", &field) || !ParseInteger(field, &count)
		|| count > kStockQueueMaximumEntries) return result;
	if (status == "unavailable") {
		if (count != 0 || cursor + 1 != lines.size()) return result;
		result.status = StockQueueReadStatus::Unavailable;
		result.canonical_response = response;
		return result;
	}
	if (lines.size() != cursor + count + 1) return result;
	result.entries.reserve(count);
	std::size_t total_parameters = 0;
	for (std::size_t index = 0; index < count; ++index) {
		if (!Field(lines[cursor++], "row", &field)) return StockQueueReadResult{};
		std::vector<std::string_view> parts;
		SplitExact(field, '|', &parts);
		if (parts.size() != 5 || parts[0] != StockQueueDomainName(expected_domain)) {
			return StockQueueReadResult{};
		}
		StockQueueEntry entry;
		if (!ParseInteger(parts[1], &entry.command_id)
			|| !ParseInteger(parts[2], &entry.options)
			|| !ParseInteger(parts[3], &entry.tag)) return StockQueueReadResult{};
		if (!parts[4].empty()) {
			std::vector<std::string_view> parameters;
			SplitExact(parts[4], ',', &parameters);
			if (parameters.size() > kStockQueueMaximumParametersPerEntry) {
				return StockQueueReadResult{};
			}
			for (auto encoded : parameters) {
				float value = 0;
				if (!ParseFloatBits(encoded, &value)) return StockQueueReadResult{};
				entry.params.push_back(value);
			}
		}
		total_parameters += entry.params.size();
		if (total_parameters > kStockQueueMaximumParameters) return StockQueueReadResult{};
		result.entries.push_back(std::move(entry));
	}
	result.status = StockQueueReadStatus::Complete;
	result.canonical_response = response;
	return result;
}

StockQueueReadResult ReadStockQueue(StockQueueDomain domain,std::int32_t unit_id,
		const StockQueueCallRules& call_rules) {
	if (!call_rules) return {};
	const auto request = BuildStockQueueRequest(domain, unit_id);
	if (request.empty()) return {};
	try {
		auto result = ParseStockQueueResponse(call_rules(request.data(), request.size()),
			request, domain, unit_id);
		result.domain = domain; result.unit_id = unit_id;
		result.canonical_request = request;
		return result;
	} catch (...) {
		StockQueueReadResult result; result.domain = domain; result.unit_id = unit_id;
		result.canonical_request = request; return result;
	}
}

}  // namespace circuit::grpc
