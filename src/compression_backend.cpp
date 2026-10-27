#include "compresso/compression_backend.h"

#include "compresso/directory_scanner.h"
#include "compresso/filesystem_compression.h"

#include <QProcess>
#include <QCryptographicHash>
#include <QFile>

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

bool readFileStamp(const std::filesystem::path& path, FileStamp& stamp,
                   DWORD* nativeError = nullptr)
{
    // Attribute write access rejects many busy-file cases while allowing other readers.
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (nativeError != nullptr) {
            *nativeError = GetLastError();
        }
        return false;
    }

    const bool okay = GetFileInformationByHandle(handle, &stamp.information) != FALSE;
    if (!okay && nativeError != nullptr) {
        *nativeError = GetLastError();
    }
    CloseHandle(handle);
    return okay;
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

bool sameFileIdentityAndSize(const FileStamp& left, const FileStamp& right)
{
    const auto& a = left.information;
    const auto& b = right.information;
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber
           && a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow
           && a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow;
}

QByteArray fileDigest(const std::filesystem::path& path)
{
    QFile file(QString::fromStdWString(path.native()));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return hash.result();
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

void attachError(compresso::CompressionFileResult& result,
                 compresso::OperationErrorCode code, std::string details,
                 int nativeCode = 0, int exitCode = -1)
{
    result.error_message = QString::fromStdString(compresso::operationErrorMessage(code));
    compresso::OperationError issue;
    issue.code = code;
    issue.path = result.path;
    issue.native_code = nativeCode;
    issue.process_exit_code = exitCode;
    issue.technical_details = std::move(details);
    result.error = std::move(issue);
}

compresso::CompressionFileResult aborted(const std::filesystem::path& path)
{
    return skipped(path, compresso::CompressionSkipReason::operation_aborted,
                   "Not attempted because an earlier serious error stopped the operation.");
}

compresso::CompressionFileResult cancelledBeforeStart(const std::filesystem::path& path)
{
    return skipped(path, compresso::CompressionSkipReason::operation_cancelled,
                   "Not attempted because the operation was cancelled.");
}

compresso::CompressionFileResult failed(const std::filesystem::path& path, QString error)
{
    compresso::CompressionFileResult result;
    result.path = path;
    result.state = compresso::CompressionFileState::failed;
    result.error_message = std::move(error);
    compresso::OperationError issue;
    issue.code = compresso::OperationErrorCode::unexpected_failure;
    issue.path = path;
    issue.technical_details = result.error_message.toStdString();
    result.error = std::move(issue);
    return result;
}

void reject(compresso::CompressionReport& report, compresso::OperationErrorCode code,
            QString message, int nativeCode = 0, std::string details = {})
{
    report.rejection_reason = std::move(message);
    compresso::OperationError issue;
    issue.code = code;
    issue.path = report.target_directory;
    issue.native_code = nativeCode;
    issue.technical_details = details.empty() ? report.rejection_reason.toStdString()
                                              : std::move(details);
    report.error = std::move(issue);
}

void setFileError(compresso::CompressionFileResult& result,
                  compresso::OperationErrorCode code, std::string details,
                  int nativeCode = 0, int exitCode = -1)
{
    attachError(result, code, std::move(details), nativeCode, exitCode);
}

std::string processDetails(const compresso::CompactProcessResult& process)
{
    std::string details;
    if (!process.error_message.isEmpty()) {
        details = process.error_message.toStdString();
    }
    if (process.exit_code >= 0) {
        if (!details.empty()) details += "\n";
        details += "compact.exe exit code: " + std::to_string(process.exit_code);
    }
    if (!process.standard_output.isEmpty()) {
        if (!details.empty()) details += "\n";
        details += "stdout:\n"
                   + QString::fromLocal8Bit(process.standard_output).toUtf8().toStdString();
    }
    if (!process.standard_error.isEmpty()) {
        if (!details.empty()) details += "\n";
        details += "stderr:\n"
                   + QString::fromLocal8Bit(process.standard_error).toUtf8().toStdString();
    }
    return details;
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
                                           const std::filesystem::path& file,
                                           bool restoring, std::stop_token cancellation)
{
    QProcess process;
    process.setProgram(QString::fromStdWString(executable.native()));
    process.setArguments({restoring ? QStringLiteral("/U") : QStringLiteral("/C"),
                          QStringLiteral("/EXE:LZX"), QStringLiteral("/Q"),
                          QString::fromStdWString(file.native())});
    process.start();

    compresso::CompactProcessResult result;
    while (!process.waitForStarted(100)) {
        if (cancellation.stop_requested() && process.state() != QProcess::NotRunning) {
            result.cancelled = true;
            process.kill();
            process.waitForFinished(-1);
            return result;
        }
        if (process.state() == QProcess::NotRunning) {
            result.error_message = "compact.exe could not start: " + process.errorString();
            result.error_code = compresso::OperationErrorCode::process_launch_failed;
            result.native_error = static_cast<int>(process.error());
            return result;
        }
    }
    result.started = true;
    while (!process.waitForFinished(100)) {
        if (process.state() == QProcess::NotRunning) {
            break;
        }
        if (!cancellation.stop_requested()) {
            continue;
        }

        result.cancelled = true;
        process.terminate();
        if (!process.waitForFinished(2000)) {
            process.kill();
            process.waitForFinished(-1);
        }
        break;
    }
    result.finished = !result.cancelled && process.state() == QProcess::NotRunning
                      && process.exitStatus() == QProcess::NormalExit;
    if (!result.finished) {
        result.error_message = "Could not collect the compact.exe process result: "
                               + process.errorString();
        result.error_code = compresso::OperationErrorCode::process_terminated;
        result.native_error = static_cast<int>(process.error());
    }
    result.crashed = process.error() != QProcess::UnknownError
                     && process.exitStatus() == QProcess::CrashExit;
    result.exit_code = process.exitCode();
    result.standard_output = process.readAllStandardOutput();
    result.standard_error = process.readAllStandardError();
    if (result.crashed) {
        result.error_message = "compact.exe terminated unexpectedly: " + process.errorString();
        result.error_code = compresso::OperationErrorCode::process_terminated;
        result.native_error = static_cast<int>(process.error());
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

std::size_t compresso::CompressionReport::restoredCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(files.begin(), files.end(), [](const auto& file) {
        return file.state == CompressionFileState::restored;
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

std::size_t compresso::CompressionReport::cancelledCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(files.begin(), files.end(), [](const auto& file) {
        return file.state == CompressionFileState::cancelled;
    }));
}

namespace
{

using namespace compresso;
using ProgressCallback = CompressionBackend::ProgressCallback;
using ProcessRunner = CompressionBackend::ProcessRunner;

compresso::CompressionReport runDirectoryOperation(
    const std::filesystem::path& directory, ProgressCallback progress,
    ProcessRunner processRunner, bool restoring, std::stop_token cancellation)
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
        const bool attributesUnavailable = selectedAttributes == INVALID_FILE_ATTRIBUTES;
        const DWORD error = attributesUnavailable ? GetLastError() : ERROR_DIRECTORY;
        reject(report, attributesUnavailable
                           ? classifyWindowsError(error)
                           : OperationErrorCode::missing_directory,
               attributesUnavailable
                   ? QString::fromStdString(operationErrorMessage(classifyWindowsError(error)))
                   : QStringLiteral("Choose an accessible game directory."),
               static_cast<int>(error),
               std::system_category().message(static_cast<int>(error)));
        return report;
    }
    if ((selectedAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        reject(report, OperationErrorCode::unsafe_target,
               "Choose a game directory that is not a symbolic link or junction.");
        return report;
    }

    std::error_code canonicalError;
    const std::filesystem::path canonicalDirectory = std::filesystem::canonical(directory,
                                                                                 canonicalError);
    if (canonicalError || !std::filesystem::is_directory(canonicalDirectory, canonicalError)) {
        const OperationErrorCode code = canonicalError
            ? classifyWindowsError(static_cast<unsigned long>(canonicalError.value()))
            : OperationErrorCode::missing_directory;
        reject(report, code,
               canonicalError ? QString::fromStdString(operationErrorMessage(code))
                              : QStringLiteral("Choose an accessible game directory."),
               canonicalError.value(),
               canonicalError ? canonicalError.message() : "The path is not a directory.");
        return report;
    }

    const DWORD directoryAttributes = GetFileAttributesW(canonicalDirectory.c_str());
    if (directoryAttributes == INVALID_FILE_ATTRIBUTES
        || (directoryAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        reject(report, OperationErrorCode::unsafe_target,
               "Choose a game directory that is not a symbolic link or junction.");
        return report;
    }

    const std::wstring volumeRoot = volumeRootFor(canonicalDirectory);
    if (volumeRoot.empty()) {
        reject(report, OperationErrorCode::unsupported_filesystem,
               "The target volume could not be identified.");
        return report;
    }
    if (pathsAreEqual(canonicalDirectory, std::filesystem::path(volumeRoot))) {
        reject(report, OperationErrorCode::unsafe_target,
               "A volume root cannot be compressed.");
        return report;
    }
    if (GetDriveTypeW(volumeRoot.c_str()) != DRIVE_FIXED) {
        reject(report, OperationErrorCode::unsupported_filesystem,
               "Choose a local fixed NTFS volume.");
        return report;
    }

    std::wstring filesystemName(128, L'\0');
    DWORD volumeFlags = 0;
    if (!GetVolumeInformationW(volumeRoot.c_str(), nullptr, 0, nullptr, nullptr, &volumeFlags,
                               filesystemName.data(),
                               static_cast<DWORD>(filesystemName.size()))) {
        const DWORD error = GetLastError();
        reject(report, classifyWindowsError(error),
               "The target volume information could not be read.", static_cast<int>(error),
               std::system_category().message(static_cast<int>(error)));
        return report;
    }
    filesystemName.resize(std::char_traits<wchar_t>::length(filesystemName.c_str()));
    report.filesystem_type = filesystemName;
    if (filesystemName != L"NTFS" || (volumeFlags & FILE_FILE_COMPRESSION) == 0) {
        reject(report, OperationErrorCode::unsupported_filesystem,
               "The target volume does not support NTFS LZX compression.", 0,
               "Detected filesystem does not provide NTFS file compression.");
        return report;
    }

    const std::filesystem::path programFiles = environmentPath(L"ProgramFiles");
    const std::filesystem::path programFilesX86 = environmentPath(L"ProgramFiles(x86)");
    if ((!programFiles.empty() && pathsAreEqual(canonicalDirectory, programFiles))
        || (!programFilesX86.empty() && pathsAreEqual(canonicalDirectory, programFilesX86))) {
        reject(report, OperationErrorCode::unsafe_target,
               "Choose an application or game folder, not the Program Files root.");
        return report;
    }

    const std::array<std::filesystem::path, 9> protectedDirectories{
        getSystemDirectory(GetWindowsDirectoryW),
        getSystemDirectory(GetSystemDirectoryW),
        getSystemDirectory(GetSystemWindowsDirectoryW),
        environmentPath(L"ProgramData"),
        std::filesystem::path(volumeRoot) / L"System Volume Information",
        std::filesystem::path(volumeRoot) / L"$Recycle.Bin",
        std::filesystem::path(volumeRoot) / L"Recovery",
        programFiles.empty() ? std::filesystem::path{}
                             : programFiles / L"WindowsApps",
        programFilesX86.empty() ? std::filesystem::path{}
                                : programFilesX86 / L"WindowsApps"};
    for (const auto& protectedDirectory : protectedDirectories) {
        if (!protectedDirectory.empty() && pathIsSameOrChild(canonicalDirectory,
                                                              protectedDirectory)) {
            reject(report, OperationErrorCode::unsafe_target,
                   "Windows and protected system locations are excluded.");
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
        reject(report, OperationErrorCode::unsafe_target,
               "Compresso's installation directory is excluded.");
        return report;
    }

    const std::filesystem::path compact = compactExecutable();
    if (compact.empty() || GetFileAttributesW(compact.c_str()) == INVALID_FILE_ATTRIBUTES) {
        reject(report, OperationErrorCode::process_launch_failed,
               "Windows compact.exe could not be located.",
               static_cast<int>(GetLastError()), "compact.exe was not found in the system directory.");
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
        }, cancellation);
    report.cancelled = scan.cancelled;
    report.issues = scan.errors;
    if (!processRunner) {
        processRunner = [restoring](const auto& executable, const auto& file,
                                    std::stop_token processCancellation) {
            return runCompact(executable, file, restoring, processCancellation);
        };
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
    bool abortRemaining = false;
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
        if (report.cancelled || cancellation.stop_requested()) {
            report.cancelled = true;
            appendResult(cancelledBeforeStart(filePath), scannedFile.logical_size_bytes);
            continue;
        }
        if (abortRemaining) {
            appendResult(aborted(filePath), scannedFile.logical_size_bytes);
            continue;
        }
        notifyProgress(filePath);
        const DWORD attributes = GetFileAttributesW(filePath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            CompressionFileResult result = skipped(filePath, CompressionSkipReason::inaccessible,
                                                   "File attributes are unavailable.");
            attachError(result, classifyWindowsError(error),
                        std::system_category().message(static_cast<int>(error)),
                        static_cast<int>(error));
            appendResult(std::move(result), scannedFile.logical_size_bytes);
            continue;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            appendResult(skipped(filePath, CompressionSkipReason::reparse_point,
                                 "Symbolic links and junctions are not processed."),
                         scannedFile.logical_size_bytes);
            continue;
        }

        if (restoring) {
            const WofBackingState backingState = inspectWofBackingState(filePath);
            if (backingState == WofBackingState::unavailable) {
                appendResult(skipped(filePath, CompressionSkipReason::inaccessible,
                                     "Windows could not verify the file's LZX algorithm."),
                             scannedFile.logical_size_bytes);
                continue;
            }
            if (backingState != WofBackingState::lzx_compressed) {
                appendResult(skipped(filePath, CompressionSkipReason::not_lzx_compressed,
                                     "The file is not WOF LZX-compressed."),
                             scannedFile.logical_size_bytes);
                continue;
            }
        } else {
            const WofBackingState backingState = inspectWofBackingState(filePath);
            if (backingState == WofBackingState::unavailable) {
                appendResult(skipped(filePath, CompressionSkipReason::inaccessible,
                                     "Windows could not verify the file's compression provider."),
                             scannedFile.logical_size_bytes);
                continue;
            }
            if (backingState == WofBackingState::lzx_compressed) {
                appendResult(skipped(filePath, CompressionSkipReason::already_compressed,
                                     "The file is already WOF LZX-compressed."),
                             scannedFile.logical_size_bytes);
                continue;
            }
            if (backingState == WofBackingState::other_external_provider) {
                appendResult(skipped(filePath, CompressionSkipReason::unsupported_entry,
                                     "Files backed by other Windows providers are not changed."),
                             scannedFile.logical_size_bytes);
                continue;
            }
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
        if (!restoring && ((attributes & FILE_ATTRIBUTE_COMPRESSED) != 0
            || (scannedFile.allocated_size_available
                && scannedFile.allocated_size_bytes < scannedFile.logical_size_bytes))) {
            appendResult(skipped(filePath, CompressionSkipReason::already_compressed,
                                 "The file is already compressed or its allocation "
                                 "is below its logical size."), scannedFile.logical_size_bytes);
            continue;
        }

        FileStamp before;
        DWORD stampError = ERROR_SUCCESS;
        if (!readFileStamp(filePath, before, &stampError)) {
            const OperationErrorCode code = classifyWindowsError(stampError);
            CompressionFileResult result = skipped(filePath, CompressionSkipReason::active_or_busy,
                QString::fromStdString(operationErrorMessage(code)));
            attachError(result, code, std::system_category().message(static_cast<int>(stampError)),
                        static_cast<int>(stampError));
            appendResult(std::move(result), scannedFile.logical_size_bytes);
            continue;
        }
        if (restoring && before.information.nNumberOfLinks > 1) {
            appendResult(skipped(filePath, CompressionSkipReason::multiple_hard_links,
                                 "Files with multiple hard links are excluded from restoration."),
                         scannedFile.logical_size_bytes);
            continue;
        }
        if (!restoring && !scannedFile.allocated_size_available) {
            appendResult(skipped(filePath, CompressionSkipReason::inaccessible,
                                 "The current disk allocation could not be measured."),
                         scannedFile.logical_size_bytes);
            continue;
        }

        const QByteArray originalDigest = restoring ? fileDigest(filePath) : QByteArray{};
        if (restoring && originalDigest.isEmpty()) {
            appendResult(skipped(filePath, CompressionSkipReason::inaccessible,
                                 "The file contents could not be verified."),
                         scannedFile.logical_size_bytes);
            continue;
        }

        const CompactProcessResult process = processRunner(compact, filePath, cancellation);
        if (process.cancelled) {
            report.cancelled = true;
            CompressionFileResult result;
            result.path = filePath;
            result.state = CompressionFileState::cancelled;
            result.exit_code = process.exit_code;
            result.standard_output = process.standard_output;
            result.standard_error = process.standard_error;
            result.error_message = "compact.exe was stopped; this file's final state is unverified.";
            appendResult(std::move(result), scannedFile.logical_size_bytes);
            continue;
        }
        if (!process.started) {
            CompressionFileResult result = failed(filePath, process.error_message);
            result.exit_code = process.exit_code;
            result.standard_output = process.standard_output;
            result.standard_error = process.standard_error;
            setFileError(result, process.error_code.value_or(OperationErrorCode::process_launch_failed),
                         processDetails(process), process.native_error, process.exit_code);
            abortRemaining = true;
            appendResult(std::move(result), scannedFile.logical_size_bytes);
            continue;
        }
        if (!process.finished) {
            CompressionFileResult result = failed(filePath, process.error_message);
            result.exit_code = process.exit_code;
            result.standard_output = process.standard_output;
            result.standard_error = process.standard_error;
            setFileError(result, process.error_code.value_or(OperationErrorCode::process_terminated),
                         processDetails(process), process.native_error, process.exit_code);
            abortRemaining = true;
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
            setFileError(fileResult, OperationErrorCode::process_terminated,
                         processDetails(process), process.native_error, process.exit_code);
            abortRemaining = true;
            appendResult(std::move(fileResult), scannedFile.logical_size_bytes);
            continue;
        }
        if (process.exit_code != 0) {
            fileResult.state = CompressionFileState::failed;
            const OperationErrorCode code = process.error_code.value_or(
                OperationErrorCode::process_failed);
            setFileError(fileResult, code,
                         processDetails(process), process.native_error, process.exit_code);
            // A nonzero compact.exe exit may represent a volume-wide failure such as low
            // space. Stop here rather than attempting additional files without a reliable,
            // locale-independent way to distinguish that from a per-file rejection.
            abortRemaining = true;
            appendResult(std::move(fileResult), scannedFile.logical_size_bytes);
            continue;
        }

        FileStamp after;
        if (!readFileStamp(filePath, after)
            || (restoring ? !sameFileIdentityAndSize(before, after)
                          : !sameFileStamp(before, after))) {
            fileResult.state = CompressionFileState::failed;
            setFileError(fileResult, OperationErrorCode::file_changed,
                         restoring ? "File identity or size changed during restoration."
                                   : "File identity, size, or last-write time changed during compression.");
            appendResult(std::move(fileResult), scannedFile.logical_size_bytes);
            continue;
        }

        if (restoring) {
            const WofBackingState finalBackingState = inspectWofBackingState(filePath);
            if (finalBackingState == WofBackingState::unavailable) {
                fileResult.state = CompressionFileState::failed;
                setFileError(fileResult, OperationErrorCode::verification_failed,
                             "WOF compression metadata is unavailable after compact.exe.");
            } else if (finalBackingState != WofBackingState::physical_file) {
                fileResult.state = CompressionFileState::failed;
                setFileError(fileResult, OperationErrorCode::verification_failed,
                             finalBackingState == WofBackingState::lzx_compressed
                                 ? "WOF still reports the LZX file provider after restoration."
                                 : "A Windows external file provider remains after restoration.");
            } else if (fileDigest(filePath) != originalDigest) {
                fileResult.state = CompressionFileState::failed;
                setFileError(fileResult, OperationErrorCode::file_changed,
                             "The SHA-256 digest changed during restoration.");
            } else {
                fileResult.state = CompressionFileState::restored;
            }
            appendResult(std::move(fileResult), scannedFile.logical_size_bytes);
            continue;
        }

        const WofBackingState finalBackingState = inspectWofBackingState(filePath);
        if (finalBackingState == WofBackingState::unavailable) {
            fileResult.state = CompressionFileState::failed;
            setFileError(fileResult, OperationErrorCode::verification_failed,
                         "Windows could not verify LZX compression after compact.exe.");
        } else if (finalBackingState == WofBackingState::lzx_compressed) {
            fileResult.state = CompressionFileState::compressed;
        } else if (finalBackingState == WofBackingState::physical_file) {
            fileResult.state = CompressionFileState::skipped;
            fileResult.skip_reason = CompressionSkipReason::unsupported_entry;
        } else {
            fileResult.state = CompressionFileState::failed;
            setFileError(fileResult, OperationErrorCode::verification_failed,
                         "The file was not stored with the requested WOF LZX provider.");
        }
        appendResult(std::move(fileResult), scannedFile.logical_size_bytes);
    }

    if (!restoring && !report.cancelled) {
        const ScanResult finalScan = DirectoryScanner::scan(canonicalDirectory, {}, cancellation);
        report.cancelled = finalScan.cancelled;
        report.issues.insert(report.issues.end(), finalScan.errors.begin(), finalScan.errors.end());
        if (!finalScan.cancelled && scan.traversal_complete && scan.allocation_measurements_complete
            && finalScan.traversal_complete && finalScan.allocation_measurements_complete) {
            report.size_summary = calculateCompressionSizeSummary(
                scan.total_allocated_bytes, finalScan.total_allocated_bytes);
        } else {
            OperationError measurementError;
            measurementError.code = OperationErrorCode::inaccessible_file;
            measurementError.path = canonicalDirectory;
            measurementError.technical_details =
                "Allocated size totals are unavailable because the pre-compression scan or "
                "post-compression scan could not measure every file and directory.";
            report.size_measurement_error = measurementError;
            report.issues.push_back(std::move(measurementError));
        }
    } else if (!restoring && report.cancelled) {
        OperationError measurementError;
        measurementError.code = OperationErrorCode::unexpected_failure;
        measurementError.path = canonicalDirectory;
        measurementError.technical_details =
            "Allocated size comparison was cancelled. Previously completed file changes remain.";
        report.size_measurement_error = measurementError;
        report.issues.push_back(std::move(measurementError));
    }

    return report;
}

} // namespace

compresso::CompressionReport compresso::CompressionBackend::compressDirectory(
    const std::filesystem::path& directory, ProgressCallback progress,
    ProcessRunner processRunner, std::stop_token cancellation)
{
    return runDirectoryOperation(directory, std::move(progress), std::move(processRunner), false,
                                 cancellation);
}

compresso::CompressionReport compresso::CompressionBackend::restoreDirectory(
    const std::filesystem::path& directory, ProgressCallback progress,
    ProcessRunner processRunner, std::stop_token cancellation)
{
    return runDirectoryOperation(directory, std::move(progress), std::move(processRunner), true,
                                 cancellation);
}
