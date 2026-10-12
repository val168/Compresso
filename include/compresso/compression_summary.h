#pragma once

#include <cstdint>

namespace compresso
{

struct CompressionSizeSummary
{
    std::uintmax_t original_allocated_bytes = 0;
    std::uintmax_t final_allocated_bytes = 0;
    std::uintmax_t bytes_saved = 0;
    std::uintmax_t bytes_increased = 0;
    long double percentage_saved = 0.0L;
    bool percentage_available = true;
};

[[nodiscard]] CompressionSizeSummary calculateCompressionSizeSummary(
    std::uintmax_t originalAllocatedBytes, std::uintmax_t finalAllocatedBytes) noexcept;

} // namespace compresso
