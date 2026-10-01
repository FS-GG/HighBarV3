// SPDX-License-Identifier: GPL-2.0-only
#include "grpc/StockQueueReader.h"

#include <bit>
#include <gtest/gtest.h>

namespace {
using namespace circuit::grpc;

constexpr const char* kRequest =
	"BARC_QUEUE_REQUEST/1\nlength=00000101\nbridge=barc-stock-queue-reader-v1\n"
	"domain=production\nunit=42\nend\n";
constexpr const char* kComplete =
	"BARC_QUEUE_RESPONSE/1\nlength=00000244\nbridge=barc-stock-queue-reader-v1\n"
	"request-sha256=b01b44c1eaea587730eae794c0b87e4e05dddecfb508abd295ac1e138a0f5960\n"
	"status=ok\ndomain=production\nunit=42\ncount=1\n"
	"row=production|-710|32|41|3f800000,80000000\nend\n";
constexpr const char* kEmpty =
	"BARC_QUEUE_RESPONSE/1\nlength=00000200\nbridge=barc-stock-queue-reader-v1\n"
	"request-sha256=b01b44c1eaea587730eae794c0b87e4e05dddecfb508abd295ac1e138a0f5960\n"
	"status=ok\ndomain=production\nunit=42\ncount=0\nend\n";
constexpr const char* kUnavailable =
	"BARC_QUEUE_RESPONSE/1\nlength=00000227\nbridge=barc-stock-queue-reader-v1\n"
	"request-sha256=b01b44c1eaea587730eae794c0b87e4e05dddecfb508abd295ac1e138a0f5960\n"
	"status=unavailable\ndomain=production\nunit=42\nreason=wrong-team\ncount=0\nend\n";

std::string BoundedResponse(std::size_t count, std::size_t parameters_per_row) {
	std::string response =
		"BARC_QUEUE_RESPONSE/1\nlength=00000000\nbridge=barc-stock-queue-reader-v1\n"
		"request-sha256=b01b44c1eaea587730eae794c0b87e4e05dddecfb508abd295ac1e138a0f5960\n"
		"status=ok\ndomain=production\nunit=42\ncount=" + std::to_string(count) + "\n";
	for (std::size_t row = 0; row < count; ++row) {
		response += "row=production|1|0|" + std::to_string(row) + "|";
		for (std::size_t parameter = 0; parameter < parameters_per_row; ++parameter) {
			if (parameter != 0) response += ',';
			response += "00000000";
		}
		response += '\n';
	}
	response += "end\n";
	const auto raw_length = std::to_string(response.size());
	const std::string length(8 - raw_length.size(), '0');
	response.replace(response.find("length=") + 7, 8, length + raw_length);
	return response;
}

TEST(StockQueueReader, CanonicalRequestAndCompleteVectorAreExact) {
	EXPECT_EQ(BuildStockQueueRequest(StockQueueDomain::Production, 42), kRequest);
	EXPECT_EQ(Sha256Hex(kRequest),
		"b01b44c1eaea587730eae794c0b87e4e05dddecfb508abd295ac1e138a0f5960");
	EXPECT_EQ(Sha256Hex(kEmpty),
		"996539730370dcee4240a0d66c24a94fd9cda77ef5fea3aff07eed90cb7409bb");
	EXPECT_EQ(Sha256Hex(kComplete),
		"b9294f24faaad63777f99c44dd6201dabf41ec32772b862d657698d3c5d32ec5");
	EXPECT_EQ(Sha256Hex(kUnavailable),
		"63861e8f1682fbd8e8551a24a4c39d1217a8b98e269bd44aceaf266c144acb6e");
	EXPECT_EQ(ParseStockQueueResponse(kEmpty, kRequest,
		StockQueueDomain::Production, 42).status, StockQueueReadStatus::Complete);
	const auto result=ParseStockQueueResponse(kComplete,kRequest,
		StockQueueDomain::Production,42);
	ASSERT_EQ(result.status,StockQueueReadStatus::Complete);
	ASSERT_EQ(result.entries.size(),1u);
	EXPECT_EQ(result.entries[0].command_id,-710);
	EXPECT_EQ(result.entries[0].options,32);
	EXPECT_EQ(result.entries[0].tag,41);
	ASSERT_EQ(result.entries[0].params.size(),2u);
	EXPECT_EQ(std::bit_cast<std::uint32_t>(result.entries[0].params[0]),0x3f800000u);
	EXPECT_EQ(std::bit_cast<std::uint32_t>(result.entries[0].params[1]),0x80000000u);
}

TEST(StockQueueReader, UnavailableIsExplicitAndCannotMintRows) {
	const auto result=ParseStockQueueResponse(kUnavailable,kRequest,
		StockQueueDomain::Production,42);
	EXPECT_EQ(result.status,StockQueueReadStatus::Unavailable);
	EXPECT_EQ(result.unavailable_reason,"wrong-team");
	EXPECT_TRUE(result.entries.empty());
}

TEST(StockQueueReader, RefusesEchoFramingAndBoundViolations) {
	auto malformed=[](std::string value){return ParseStockQueueResponse(value,kRequest,
		StockQueueDomain::Production,42).status==StockQueueReadStatus::Malformed;};
	std::string value=kComplete;value.replace(value.find("unit=42"),7,"unit=43");EXPECT_TRUE(malformed(value));
	value=kComplete;value.replace(value.find("domain=production"),17,"domain=rally     ");EXPECT_TRUE(malformed(value));
	value=kComplete;value.replace(value.find("length=00000244"),15,"length=00000243");EXPECT_TRUE(malformed(value));
	value=kComplete;value[value.find("status=ok")+2]='\0';EXPECT_TRUE(malformed(value));
	value=kComplete;value.replace(value.find("3f800000"),8,"7f800000");EXPECT_TRUE(malformed(value));
	value=kComplete;value.replace(value.find("count=1"),7,"count=65");EXPECT_TRUE(malformed(value));
	value.insert(value.find("end\n"),std::string(kStockQueueMaximumLineBytes+1,'x')+"\n");EXPECT_TRUE(malformed(value));
}

TEST(StockQueueReader, EnforcesEntryAndParameterLimitsAtExactBoundary) {
	auto parse = [](const std::string& value) {
		return ParseStockQueueResponse(value, kRequest,
			StockQueueDomain::Production, 42).status;
	};
	EXPECT_EQ(parse(BoundedResponse(64, 0)), StockQueueReadStatus::Complete);
	EXPECT_EQ(parse(BoundedResponse(65, 0)), StockQueueReadStatus::Malformed);
	EXPECT_EQ(parse(BoundedResponse(1, 16)), StockQueueReadStatus::Complete);
	EXPECT_EQ(parse(BoundedResponse(1, 17)), StockQueueReadStatus::Malformed);
	EXPECT_EQ(parse(BoundedResponse(16, 16)), StockQueueReadStatus::Complete);
	EXPECT_EQ(parse(BoundedResponse(17, 16)), StockQueueReadStatus::Malformed);
}

TEST(StockQueueReader, CallsLuaExactlyOnceWithoutRetryOrFallback) {
	int calls=0;std::string observed;
	const auto result=ReadStockQueue(StockQueueDomain::Production,42,
		[&](const char* data,std::size_t size){++calls;observed.assign(data,size);return std::string(kComplete);});
	EXPECT_EQ(calls,1);EXPECT_EQ(observed,kRequest);
	EXPECT_EQ(result.status,StockQueueReadStatus::Complete);
	const auto failed=ReadStockQueue(StockQueueDomain::Production,42,
		[&](const char*,std::size_t){++calls;return std::string();});
	EXPECT_EQ(calls,2);EXPECT_EQ(failed.status,StockQueueReadStatus::Malformed);
}

}  // namespace
