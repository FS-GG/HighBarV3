// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <cctype>
#include <string>

namespace circuit::grpc {

inline bool HasCatalogueText(const char* value) {
	if (value == nullptr) return false;
	for (const unsigned char byte : std::string(value)) {
		if (!std::isspace(byte)) return true;
	}
	return false;
}

// BAR keeps user-facing unit names in its localization catalogue, so the
// engine callback can legitimately expose an empty human name.  The native
// unit name remains the stable, content-owned label in that case.
inline std::string TacticalCatalogueDisplayName(
		const char* internal_name, const char* human_name) {
	if (HasCatalogueText(human_name)) return human_name;
	return internal_name != nullptr ? internal_name : "";
}

}  // namespace circuit::grpc
