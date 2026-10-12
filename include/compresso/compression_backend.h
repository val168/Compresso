#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <QByteArray>
#include <QString>

namespace compresso
{

enum class CompressionFileState
{
    compressed,
    restored,
    skipped,
    failed
};

enum class CompressionSkipReason
{
    inaccessible,
    reparse_point,
    already_compressed,
    not_lzx_compressed,
    encrypted,
    sparse,
    active_or_busy,
    multiple_hard_links,
    unsupported_entry
};

struct CompressionFileResult
{
    std::filesystem::path path;
    CompressionFileState state = CompressionFileState::failed;
    std::optional<CompressionSkipReason> skip_reason;
    int exit_code = -1;
    QByteArray standard_output;
    QByteArray standard_error;
    QString error_message;
};

struct CompressionProgress
{
    std::size_t completed_files = 0;
    std::size_t total_files = 0;
    std::uintmax_t processed_logical_bytes = 0;
    std::uintmax_t total_logical_bytes = 0;
    std::chrono::milliseconds elapsed{};
    std::filesystem::path current_file;
    bool totals_known = false;
};

struct CompactProcessResult
{
    bool started = false;
    bool finished = false;
    bool crashed = false;
    int exit_code = -1;
    QByteArray standard_output;
    QByteArray standard_error;
    QString error_message;
};

struct CompressionReport
{
    std::filesystem::path target_directory;
    std::wstring filesystem_type;
    bool target_eligible = false;
    QString rejection_reason;
    std::vector<CompressionFileResult> files;

    [[nodiscard]] std::size_t compressedCount() const noexcept;
    [[nodiscard]] std::size_t restoredCount() const noexcept;
    [[nodiscard]] std::size_t skippedCount() const noexcept;
    [[nodiscard]] std::size_t failedCount() const noexcept;
};

class CompressionBackend final
{
public:
    using ProgressCallback = std::function<void(const CompressionProgress& progress)>;
    using ProcessRunner = std::function<CompactProcessResult(
        const std::filesystem::path& executable, const std::filesystem::path& file)>;

    [[nodiscard]] static CompressionReport compressDirectory(
        const std::filesystem::path& directory, ProgressCallback progress = {},
        ProcessRunner processRunner = {});
    [[nodiscard]] static CompressionReport restoreDirectory(
        const std::filesystem::path& directory, ProgressCallback progress = {},
        ProcessRunner processRunner = {});
};

} // namespace compresso
