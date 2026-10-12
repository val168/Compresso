#pragma once

#include "compresso/operation_error.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <stop_token>
#include <vector>

namespace compresso
{

struct FileAttributes
{
    bool read_only = false;
    bool hidden = false;
    bool system = false;
    bool archive = false;
    bool compressed = false;
    bool encrypted = false;
    bool sparse = false;
};

struct ScannedFile
{
    std::filesystem::path path;
    std::uintmax_t logical_size_bytes = 0;
    std::uintmax_t allocated_size_bytes = 0;
    bool allocated_size_available = false;
    FileAttributes attributes;
};

struct VolumeInfo
{
    std::wstring filesystem_type;
    bool supports_file_compression = false;
    bool compression_eligible = false;
    std::error_code error;
};

struct MeasurementFailure
{
    std::filesystem::path path;
    std::error_code error;
};

enum class SkipReason
{
    inaccessible_directory,
    inaccessible_entry,
    reparse_point,
    unsupported_file_type,
    file_size_error,
    traversal_error
};

struct SkippedEntry
{
    std::filesystem::path path;
    SkipReason reason = SkipReason::traversal_error;
    std::error_code error;
};

struct ScanResult
{
    std::filesystem::path root;
    VolumeInfo volume;
    std::vector<ScannedFile> files;
    std::vector<SkippedEntry> skipped_entries;
    std::vector<MeasurementFailure> allocation_measurement_failures;
    std::size_t total_file_count = 0;
    std::uintmax_t total_logical_bytes = 0;
    std::uintmax_t total_allocated_bytes = 0;
    bool allocation_measurements_complete = true;
    bool traversal_complete = true;
    bool cancelled = false;
    std::vector<OperationError> errors;
};

class DirectoryScanner final
{
public:
    using ProgressCallback =
        std::function<void(std::size_t entries_visited, std::size_t files_found)>;

    [[nodiscard]] static ScanResult scan(const std::filesystem::path& root,
                                         ProgressCallback progress = {},
                                         std::stop_token cancellation = {});
};

} // namespace compresso
