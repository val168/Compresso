#pragma once

#include <filesystem>

namespace compresso
{

enum class WofBackingState
{
    lzx_compressed,
    other_external_provider,
    physical_file,
    unavailable
};

enum class FileCompressionState
{
    unknown,
    uncompressed,
    lzx_compressed,
    other
};

[[nodiscard]] WofBackingState inspectWofBackingState(const std::filesystem::path& path);
[[nodiscard]] FileCompressionState inspectFileCompressionState(
    const std::filesystem::path& path, bool ntfsCompressedAttribute,
    bool allocationSuggestsCompression);

} // namespace compresso
