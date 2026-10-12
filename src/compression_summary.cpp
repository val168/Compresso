#include "compresso/compression_summary.h"

namespace compresso
{

CompressionSizeSummary calculateCompressionSizeSummary(
    std::uintmax_t originalAllocatedBytes, std::uintmax_t finalAllocatedBytes) noexcept
{
    CompressionSizeSummary summary;
    summary.original_allocated_bytes = originalAllocatedBytes;
    summary.final_allocated_bytes = finalAllocatedBytes;
    summary.bytes_saved = originalAllocatedBytes > finalAllocatedBytes
        ? originalAllocatedBytes - finalAllocatedBytes
        : 0;
    summary.bytes_increased = finalAllocatedBytes > originalAllocatedBytes
        ? finalAllocatedBytes - originalAllocatedBytes
        : 0;

    if (originalAllocatedBytes == 0) {
        summary.percentage_available = finalAllocatedBytes == 0;
        return summary;
    }

    const long double difference = static_cast<long double>(originalAllocatedBytes)
                                   - static_cast<long double>(finalAllocatedBytes);
    summary.percentage_saved = difference * 100.0L
                               / static_cast<long double>(originalAllocatedBytes);
    return summary;
}

} // namespace compresso
