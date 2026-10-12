#pragma once

#include <filesystem>
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
    skipped,
    failed
};

enum class CompressionSkipReason
{
    inaccessible,
    reparse_point,
    already_compressed,
    encrypted,
    sparse,
    active_or_busy,
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

struct CompressionReport
{
    std::filesystem::path target_directory;
    std::wstring filesystem_type;
    bool target_eligible = false;
    QString rejection_reason;
    std::vector<CompressionFileResult> files;

    [[nodiscard]] std::size_t compressedCount() const noexcept;
    [[nodiscard]] std::size_t skippedCount() const noexcept;
    [[nodiscard]] std::size_t failedCount() const noexcept;
};

class CompressionBackend final
{
public:
    [[nodiscard]] static CompressionReport compressDirectory(
        const std::filesystem::path& directory);
};

} // namespace compresso
