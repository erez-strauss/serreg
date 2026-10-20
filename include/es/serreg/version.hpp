// es/serreg/version.hpp — the library version as a string, for the tools and the writer-info frame (1.7).
// Changed only by scripts/bump-version.sh.
#pragma once
#include <string_view>

namespace es::serreg
{
inline constexpr std::string_view library_version = "2.2.6";
} // namespace es::serreg
