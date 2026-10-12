#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <system_error>
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
    FileAttributes attributes;
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
    std::vector<ScannedFile> files;
    std::vector<SkippedEntry> skipped_entries;
    std::size_t total_file_count = 0;
    std::uintmax_t total_directory_size_bytes = 0;
    bool traversal_complete = true;
};

class DirectoryScanner final
{
public:
    [[nodiscard]] static ScanResult scan(const std::filesystem::path& root);
};

} // namespace compresso
