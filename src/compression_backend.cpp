#include "compresso/compression_backend.h"

#include "compresso/directory_scanner.h"

#include <QProcess>

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace
{

std::wstring trimTrailingSeparators(std::wstring value)
{
    while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
    }
    return value;
}

bool pathIsSameOrChild(const std::filesystem::path& candidate,
                       const std::filesystem::path& parent)
{
    const std::wstring candidateText = trimTrailingSeparators(
        candidate.lexically_normal().native());
    const std::wstring parentText = trimTrailingSeparators(parent.lexically_normal().native());
    if (candidateText.empty() || parentText.empty() || candidateText.size() < parentText.size()) {
        return false;
    }

    if (CompareStringOrdinal(candidateText.c_str(), static_cast<int>(parentText.size()),
                             parentText.c_str(), static_cast<int>(parentText.size()), TRUE)
        != CSTR_EQUAL) {
        return false;
    }
    return candidateText.size() == parentText.size()
           || parentText.back() == L'\\'
           || candidateText[parentText.size()] == L'\\';
}

bool pathsAreEqual(const std::filesystem::path& left, const std::filesystem::path& right)
{
    const std::wstring leftText = trimTrailingSeparators(left.lexically_normal().native());
    const std::wstring rightText = trimTrailingSeparators(right.lexically_normal().native());
    return leftText.size() == rightText.size()
           && CompareStringOrdinal(leftText.c_str(), static_cast<int>(leftText.size()),
                                   rightText.c_str(), static_cast<int>(rightText.size()), TRUE)
               == CSTR_EQUAL;
}

template <typename DirectoryFunction>
std::filesystem::path getSystemDirectory(DirectoryFunction function)
{
    std::wstring buffer(32768, L'\0');
    const UINT length = function(buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

std::filesystem::path environmentPath(const wchar_t* name)
{
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return {};
    }
    std::wstring value(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), required);
    if (length == 0 || length >= required) {
        return {};
    }
    value.resize(length);
    return std::filesystem::path(value);
}

std::filesystem::path applicationDirectory()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::wstring volumeRootFor(const std::filesystem::path& directory)
{
    std::wstring volumeRoot(32768, L'\0');
    if (!GetVolumePathNameW(directory.c_str(), volumeRoot.data(),
                            static_cast<DWORD>(volumeRoot.size()))) {
        return {};
    }
    volumeRoot.resize(std::char_traits<wchar_t>::length(volumeRoot.c_str()));
    return volumeRoot;
}

std::filesystem::path compactExecutable()
{
    const std::filesystem::path systemDirectory = getSystemDirectory(GetSystemDirectoryW);
    return systemDirectory.empty() ? std::filesystem::path{}
                                   : systemDirectory / L"compact.exe";
}

struct FileStamp
{
    BY_HANDLE_FILE_INFORMATION information{};
};

bool readFileStamp(const std::filesystem::path& path, FileStamp& stamp)
{
    // Write-attribute access with no write sharing rejects common busy-file cases while
    // releasing the handle before compact.exe needs the file.
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    const bool okay = GetFileInformationByHandle(handle, &stamp.information) != FALSE;
    CloseHandle(handle);
    return okay;
}

bool getAllocatedSize(const std::filesystem::path& path, std::uintmax_t& size)
{
    DWORD high = 0;
    SetLastError(ERROR_SUCCESS);
    const DWORD low = GetCompressedFileSizeW(path.c_str(), &high);
    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
        return false;
    }
    size = (static_cast<std::uintmax_t>(high) << 32) | low;
    return true;
}

bool sameFileStamp(const FileStamp& left, const FileStamp& right)
{
    const auto& a = left.information;
    const auto& b = right.information;
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber
           && a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow
           && a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow
           && a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime
           && a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime;
}

compresso::CompressionFileResult skipped(const std::filesystem::path& path,
                                         compresso::CompressionSkipReason reason,
                                         QString error = {})
{
    compresso::CompressionFileResult result;
    result.path = path;
    result.state = compresso::CompressionFileState::skipped;
    result.skip_reason = reason;
    result.error_message = std::move(error);
    return result;
}

compresso::CompressionFileResult failed(const std::filesystem::path& path, QString error)
{
    compresso::CompressionFileResult result;
    result.path = path;
    result.state = compresso::CompressionFileState::failed;
    result.error_message = std::move(error);
    return result;
}

QString skipReasonMessage(compresso::SkipReason reason)
{
    using compresso::SkipReason;
    switch (reason) {
    case SkipReason::inaccessible_directory:
    case SkipReason::inaccessible_entry:
        return "The path is inaccessible.";
    case SkipReason::reparse_point:
        return "Symbolic links and junctions are not compressed.";
    case SkipReason::unsupported_file_type:
        return "Unsupported filesystem entry.";
    case SkipReason::file_size_error:
        return "File metadata could not be read.";
    case SkipReason::traversal_error:
        return "The directory could not be fully scanned.";
    }
    return "The entry was skipped.";
}

compresso::CompactProcessResult runCompact(const std::filesystem::path& executable,
                                           const std::filesystem::path& file)
{
    QProcess process;
    process.setProgram(QString::fromStdWString(executable.native()));
    process.setArguments({QStringLiteral("/C"), QStringLiteral("/EXE:LZX"),
                          QStringLiteral("/Q"), QString::fromStdWString(file.native())});
    process.start();

    compresso::CompactProcessResult result;
    if (!process.waitForStarted()) {
        result.error_message = "compact.exe could not start: " + process.errorString();
        return result;
    }
    result.started = true;
    result.finished = process.waitForFinished(-1);
    if (!result.finished) {
        result.error_message = "Could not collect the compact.exe process result: "
                               + process.errorString();
    }
    result.crashed = process.error() != QProcess::UnknownError
                     && process.exitStatus() == QProcess::CrashExit;
    result.exit_code = process.exitCode();
    result.standard_output = process.readAllStandardOutput();
    result.standard_error = process.readAllStandardError();
    if (result.crashed) {
        result.error_message = "compact.exe terminated unexpectedly: " + process.errorString();
    }
    return result;
}

} // namespace

std::size_t compresso::CompressionReport::compressedCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(files.begin(), files.end(), [](const auto& file) {
        return file.state == CompressionFileState::compressed;
    }));
}

std::size_t compresso::CompressionReport::skippedCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(files.begin(), files.end(), [](const auto& file) {
        return file.state == CompressionFileState::skipped;
    }));
}

std::size_t compresso::CompressionReport::failedCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(files.begin(), files.end(), [](const auto& file) {
        return file.state == CompressionFileState::failed;
    }));
}

compresso::CompressionReport compresso::CompressionBackend::compressDirectory(
    const std::filesystem::path& directory, ProgressCallback progress,
    ProcessRunner processRunner)
{
    const auto startedAt = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt);
    };
    CompressionReport report;
    report.target_directory = directory;

    const DWORD selectedAttributes = GetFileAttributesW(directory.c_str());
    if (selectedAttributes == INVALID_FILE_ATTRIBUTES
        || (selectedAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        report.rejection_reason = "Choose an accessible game directory.";
        return report;
    }
    if ((selectedAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        report.rejection_reason = "Symbolic links and junctions cannot be compressed.";
        return report;
    }

    std::error_code canonicalError;
    const std::filesystem::path canonicalDirectory = std::filesystem::canonical(directory,
                                                                                 canonicalError);
    if (canonicalError || !std::filesystem::is_directory(canonicalDirectory, canonicalError)) {
        report.rejection_reason = "Choose an accessible game directory.";
        return report;
    }

    const DWORD directoryAttributes = GetFileAttributesW(canonicalDirectory.c_str());
    if (directoryAttributes == INVALID_FILE_ATTRIBUTES
        || (directoryAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        report.rejection_reason = "Symbolic links and junctions cannot be compressed.";
        return report;
    }

    const std::wstring volumeRoot = volumeRootFor(canonicalDirectory);
    if (volumeRoot.empty()) {
        report.rejection_reason = "The target volume could not be identified.";
        return report;
    }
    if (pathsAreEqual(canonicalDirectory, std::filesystem::path(volumeRoot))) {
        report.rejection_reason = "A volume root cannot be compressed.";
        return report;
    }
    if (GetDriveTypeW(volumeRoot.c_str()) != DRIVE_FIXED) {
        report.rejection_reason = "Choose a local fixed NTFS volume.";
        return report;
    }

    std::wstring filesystemName(128, L'\0');
    DWORD volumeFlags = 0;
    if (!GetVolumeInformationW(volumeRoot.c_str(), nullptr, 0, nullptr, nullptr, &volumeFlags,
                               filesystemName.data(),
                               static_cast<DWORD>(filesystemName.size()))) {
        report.rejection_reason = "The target volume information could not be read.";
        return report;
    }
    filesystemName.resize(std::char_traits<wchar_t>::length(filesystemName.c_str()));
    report.filesystem_type = filesystemName;
    if (filesystemName != L"NTFS" || (volumeFlags & FILE_FILE_COMPRESSION) == 0) {
        report.rejection_reason = "The target volume does not support NTFS LZX compression.";
        return report;
    }

    const std::array<std::filesystem::path, 9> protectedDirectories{
        getSystemDirectory(GetWindowsDirectoryW),
        getSystemDirectory(GetSystemDirectoryW),
        getSystemDirectory(GetSystemWindowsDirectoryW),
        environmentPath(L"ProgramFiles"),
        environmentPath(L"ProgramFiles(x86)"),
        environmentPath(L"ProgramData"),
        std::filesystem::path(volumeRoot) / L"System Volume Information",
        std::filesystem::path(volumeRoot) / L"$Recycle.Bin",
        std::filesystem::path(volumeRoot) / L"Recovery"};
    for (const auto& protectedDirectory : protectedDirectories) {
        if (!protectedDirectory.empty() && pathIsSameOrChild(canonicalDirectory,
                                                              protectedDirectory)) {
            report.rejection_reason = "Windows and protected system locations are excluded.";
            return report;
        }
    }

    const std::filesystem::path installDirectory = applicationDirectory();
    std::error_code installCanonicalError;
    const std::filesystem::path canonicalInstallDirectory = installDirectory.empty()
        ? std::filesystem::path{}
        : std::filesystem::canonical(installDirectory, installCanonicalError);
    if (!canonicalInstallDirectory.empty()
        && !installCanonicalError
        && (pathIsSameOrChild(canonicalDirectory, canonicalInstallDirectory)
            || pathIsSameOrChild(canonicalInstallDirectory, canonicalDirectory))) {
        report.rejection_reason = "Compresso's installation directory is excluded.";
        return report;
    }

    const std::filesystem::path compact = compactExecutable();
    if (compact.empty() || GetFileAttributesW(compact.c_str()) == INVALID_FILE_ATTRIBUTES) {
        report.rejection_reason = "Windows compact.exe could not be located.";
        return report;
    }

    report.target_eligible = true;
    const ScanResult scan = DirectoryScanner::scan(
        canonicalDirectory, [&progress, &elapsed](std::size_t, std::size_t) {
            if (progress) {
                CompressionProgress update;
                update.elapsed = elapsed();
                progress(update);
            }
        });
    if (!processRunner) {
        processRunner = runCompact;
    }
    const std::size_t totalFiles = scan.files.size();
    std::size_t completedFiles = 0;
    std::uintmax_t processedBytes = 0;
    const auto notifyProgress = [&](const std::filesystem::path& currentFile = {}) {
        if (progress) {
            CompressionProgress update;
            update.completed_files = completedFiles;
            update.total_files = totalFiles;
            update.processed_logical_bytes = processedBytes;
            update.total_logical_bytes = scan.total_logical_bytes;
            update.elapsed = elapsed();
            update.current_file = currentFile;
            update.totals_known = true;
            progress(update);
        }
    };
    notifyProgress();
    const auto appendResult = [&](CompressionFileResult result, std::uintmax_t logicalSize) {
        report.files.push_back(std::move(result));
        ++completedFiles;
        processedBytes += logicalSize;
        notifyProgress();
    };
    for (const auto& entry : scan.skipped_entries) {
        const CompressionSkipReason reason = entry.reason == SkipReason::reparse_point
            ? CompressionSkipReason::reparse_point
            : entry.reason == SkipReason::unsupported_file_type
                ? CompressionSkipReason::unsupported_entry
                : CompressionSkipReason::inaccessible;
        report.files.push_back(skipped(entry.path, reason, skipReasonMessage(entry.reason)));
    }

    for (const auto& scannedFile : scan.files) {
        const std::filesystem::path filePath = scannedFile.path;
        notifyProgress(filePath);
        const DWORD attributes = GetFileAttributesW(filePath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            appendResult(skipped(filePath, CompressionSkipReason::inaccessible,
                                 "File attributes are unavailable."), scannedFile.logical_size_bytes);
            continue;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            appendResult(skipped(filePath, CompressionSkipReason::reparse_point,
                                 "Symbolic links and junctions are not compressed."),
                         scannedFile.logical_size_bytes);
            continue;
        }
        if ((attributes & FILE_ATTRIBUTE_ENCRYPTED) != 0) {
            appendResult(skipped(filePath, CompressionSkipReason::encrypted,
                                 "Encrypted files are not compressed."), scannedFile.logical_size_bytes);
            continue;
        }
        if ((attributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0) {
            appendResult(skipped(filePath, CompressionSkipReason::sparse,
                                 "Sparse files are not compressed."), scannedFile.logical_size_bytes);
            continue;
        }
        if ((attributes & FILE_ATTRIBUTE_COMPRESSED) != 0
            || (scannedFile.allocated_size_available
                && scannedFile.allocated_size_bytes < scannedFile.logical_size_bytes)) {
            appendResult(skipped(filePath, CompressionSkipReason::already_compressed,
                                 "The file is already compressed or its allocation "
                                 "is below its logical size."), scannedFile.logical_size_bytes);
            continue;
        }

        FileStamp before;
        if (!readFileStamp(filePath, before)) {
            appendResult(skipped(filePath, CompressionSkipReason::active_or_busy,
                                 "The file is inaccessible, write-protected, or open for writing."),
                         scannedFile.logical_size_bytes);
            continue;
        }
        if (!scannedFile.allocated_size_available) {
            appendResult(skipped(filePath, CompressionSkipReason::inaccessible,
                                 "The current disk allocation could not be measured."),
                         scannedFile.logical_size_bytes);
            continue;
        }

        const CompactProcessResult process = processRunner(compact, filePath);
        if (!process.started) {
            appendResult(failed(filePath, process.error_message), scannedFile.logical_size_bytes);
            continue;
        }
        if (!process.finished) {
            CompressionFileResult result = failed(filePath, process.error_message);
            result.exit_code = process.exit_code;
            result.standard_output = process.standard_output;
            result.standard_error = process.standard_error;
            appendResult(std::move(result), scannedFile.logical_size_bytes);
            continue;
        }

        CompressionFileResult fileResult;
        fileResult.path = filePath;
        fileResult.exit_code = process.exit_code;
        fileResult.standard_output = process.standard_output;
        fileResult.standard_error = process.standard_error;

        if (process.crashed) {
            fileResult.state = CompressionFileState::failed;
            fileResult.error_message = process.error_message;
            appendResult(std::move(fileResult), scannedFile.logical_size_bytes);
            continue;
        }
        if (process.exit_code != 0) {
            fileResult.state = CompressionFileState::failed;
            fileResult.error_message = "compact.exe reported that compression failed.";
            appendResult(std::move(fileResult), scannedFile.logical_size_bytes);
            continue;
        }

        FileStamp after;
        if (!readFileStamp(filePath, after) || !sameFileStamp(before, after)) {
            fileResult.state = CompressionFileState::failed;
            fileResult.error_message = "The file changed or became busy during compression.";
            appendResult(std::move(fileResult), scannedFile.logical_size_bytes);
            continue;
        }

        const DWORD finalAttributes = GetFileAttributesW(filePath.c_str());
        std::uintmax_t finalAllocatedSize = 0;
        const bool finalAllocationAvailable = getAllocatedSize(filePath, finalAllocatedSize);
        const bool allocationReduced = finalAllocationAvailable
                                       && finalAllocatedSize < scannedFile.allocated_size_bytes;
        if (finalAttributes == INVALID_FILE_ATTRIBUTES) {
            fileResult.state = CompressionFileState::failed;
            fileResult.error_message = "The file could not be inspected after compression.";
        } else if ((finalAttributes & FILE_ATTRIBUTE_COMPRESSED) != 0 || allocationReduced) {
            fileResult.state = CompressionFileState::compressed;
        } else if (finalAllocationAvailable) {
            fileResult.state = CompressionFileState::skipped;
            fileResult.skip_reason = CompressionSkipReason::unsupported_entry;
            fileResult.error_message = "The filesystem did not retain LZX compression for this file.";
        } else {
            fileResult.state = CompressionFileState::failed;
            fileResult.error_message = "The filesystem did not provide a final allocation measurement.";
        }
        appendResult(std::move(fileResult), scannedFile.logical_size_bytes);
    }

    return report;
}
