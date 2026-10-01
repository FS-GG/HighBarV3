// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "highbar/live_control.pb.h"

#include "ExternalAI/Interface/AISCommands.h"

namespace circuit::grpc {

constexpr short TacticalOptions(::highbar::v1::NativeQueuePolicy policy) {
	return policy == ::highbar::v1::NATIVE_QUEUE_POLICY_APPEND
		? UNIT_COMMAND_OPTION_SHIFT_KEY : 0;
}

// Stock Recoil interprets SHIFT on a factory build command as a quantity
// multiplier of five. Queue append is already the factory's default, so the
// admitted count-one intent must reach the engine without modifier options.
constexpr short StockFactoryProductionOptions() {
	return 0;
}

}  // namespace circuit::grpc
