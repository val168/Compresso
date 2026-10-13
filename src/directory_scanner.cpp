#include "compresso/directory_scanner.h"

#include <filesystem>
#include <limits>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <string>
#endif

namespace
{

struct EntryMetadata
{
    bool is_directory = false;
    bool is_regular_file = false;
    bool is_reparse_point = false;
    compresso::FileAttributes attributes;
};

bool readMetadata(const std::filesystem::path& path, EntryMetadata& metadata,
                  std::error_code& error)
{
#ifdef _WIN32
    const DWORD nativeAttributes = GetFileAttributesW(path.c_str());
    if (nativeAttributes == INVALID_FILE_ATTRIBUTES) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }

    metadata.is_directory = (nativeAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    metadata.is_reparse_point = (nativeAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    metadata.attributes.read_only = (nativeAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    metadata.attributes.hidden = (nativeAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
    metadata.attributes.system = (nativeAttributes & FILE_ATTRIBUTE_SYSTEM) != 0;
    metadata.attributes.archive = (nativeAttributes & FILE_ATTRIBUTE_ARCHIVE) != 0;
    metadata.attributes.compressed = (nativeAttributes & FILE_ATTRIBUTE_COMPRESSED) != 0;
    metadata.attributes.encrypted = (nativeAttributes & FILE_ATTRIBUTE_ENCRYPTED) != 0;
    metadata.attributes.sparse = (nativeAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0;

    if (!metadata.is_directory && !metadata.is_reparse_point) {
        std::error_code statusError;
        const auto status = std::filesystem::symlink_status(path, statusError);
        if (statusError) {
            error = statusError;
            return false;
        }
        metadata.is_regular_file = std::filesystem::is_regular_file(status);
    }
#else
    const auto status = std::filesystem::symlink_status(path, error);
    if (error) {
        return false;
    }

    metadata.is_reparse_point = std::filesystem::is_symlink(status);
    metadata.is_directory = std::filesystem::is_directory(status);
    metadata.is_regular_file = std::filesystem::is_regular_file(status);
    metadata.attributes.read_only =
        (status.permissions() & std::filesystem::perms::owner_write) == std::filesystem::perms::none;
    metadata.attributes.hidden = path.filename().native().starts_with('.');
#endif
    return true;
}

void skip(compresso::ScanResult& result, const std::filesystem::path& path,
          compresso::SkipReason reason, const std::error_code& error = {})
{
    result.skipped_entries.push_back({path, reason, error});
    if (reason == compresso::SkipReason::inaccessible_directory
        || reason == compresso::SkipReason::inaccessible_entry
        || reason == compresso::SkipReason::file_size_error
        || reason == compresso::SkipReason::traversal_error) {
        compresso::OperationError issue;
        issue.code = error
            ? compresso::classifyWindowsError(static_cast<unsigned long>(error.value()))
            : compresso::OperationErrorCode::inaccessible_file;
        issue.path = path;
        issue.native_code = error.value();
        issue.technical_details = error ? error.message() : "Filesystem entry was inaccessible.";
        result.errors.push_back(std::move(issue));
    }
}

compresso::VolumeInfo inspectVolume(const std::filesystem::path& root)
{
    compresso::VolumeInfo info;
#ifdef _WIN32
    std::wstring volumePath(32768, L'\0');
    if (!GetVolumePathNameW(root.c_str(), volumePath.data(),
                            static_cast<DWORD>(volumePath.size()))) {
        info.error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return info;
    }

    std::wstring filesystemName(128, L'\0');
    DWORD volumeFlags = 0;
    if (!GetVolumeInformationW(volumePath.c_str(), nullptr, 0, nullptr, nullptr,
                               &volumeFlags, filesystemName.data(),
                               static_cast<DWORD>(filesystemName.size()))) {
        info.error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return info;
    }

    filesystemName.resize(std::char_traits<wchar_t>::length(filesystemName.c_str()));
    info.filesystem_type = std::move(filesystemName);
    info.supports_file_compression = (volumeFlags & FILE_FILE_COMPRESSION) != 0;
    info.compression_eligible = info.filesystem_type == L"NTFS"
                                && info.supports_file_compression;
#else
    (void)root;
    info.error = std::make_error_code(std::errc::operation_not_supported);
#endif
    return info;
}

bool getAllocatedSize(const std::filesystem::path& path, std::uintmax_t& size,
                      std::error_code& error)
{
#ifdef _WIN32
    DWORD high = 0;
    SetLastError(ERROR_SUCCESS);
    const DWORD low = GetCompressedFileSizeW(path.c_str(), &high);
    if (low == INVALID_FILE_SIZE) {
        const DWORD code = GetLastError();
        if (code != NO_ERROR) {
            error = std::error_code(static_cast<int>(code), std::system_category());
            return false;
        }
    }
    size = (static_cast<std::uintmax_t>(high) << 32) | low;
    return true;
#else
    (void)path;
    (void)size;
    error = std::make_error_code(std::errc::operation_not_supported);
    return false;
#endif
}

} // namespace

compresso::ScanResult compresso::DirectoryScanner::scan(const std::filesystem::path& root,
                                                         ProgressCallback progress,
                                                         std::stop_token cancellation)
{
    ScanResult result;
    result.root = root;
    result.volume = inspectVolume(root);
    if (result.volume.error) {
        OperationError issue;
        issue.code = classifyWindowsError(static_cast<unsigned long>(result.volume.error.value()));
        issue.path = root;
        issue.native_code = result.volume.error.value();
        issue.technical_details = result.volume.error.message();
        result.errors.push_back(std::move(issue));
    } else if (!result.volume.compression_eligible) {
        OperationError issue;
        issue.code = OperationErrorCode::unsupported_filesystem;
        issue.path = root;
        issue.technical_details = result.volume.filesystem_type.empty()
            ? "Filesystem metadata is unavailable."
            : "The filesystem does not advertise support for NTFS file compression.";
        result.errors.push_back(std::move(issue));
    }
    std::size_t entriesVisited = 0;
    const auto reportProgress = [&] {
        if (progress) {
            progress(entriesVisited, result.files.size());
        }
    };

    EntryMetadata rootMetadata;
    std::error_code rootError;
    if (!readMetadata(root, rootMetadata, rootError)) {
        result.traversal_complete = false;
        skip(result, root, SkipReason::inaccessible_directory, rootError);
        return result;
    }
    if (rootMetadata.is_reparse_point) {
        result.traversal_complete = false;
        skip(result, root, SkipReason::reparse_point);
        return result;
    }
    if (!rootMetadata.is_directory) {
        result.traversal_complete = false;
        skip(result, root, SkipReason::unsupported_file_type);
        return result;
    }

    std::vector<std::filesystem::path> pendingDirectories{root};
    while (!pendingDirectories.empty() && !cancellation.stop_requested()) {
        std::filesystem::path directory = std::move(pendingDirectories.back());
        pendingDirectories.pop_back();

        std::error_code iteratorError;
        std::filesystem::directory_iterator iterator(directory, iteratorError);
        const std::filesystem::directory_iterator end;
        if (iteratorError) {
            result.traversal_complete = false;
            skip(result, directory, SkipReason::inaccessible_directory, iteratorError);
            continue;
        }

        while (iterator != end && !cancellation.stop_requested()) {
            ++entriesVisited;
            const std::filesystem::path entryPath = iterator->path();
            EntryMetadata metadata;
            std::error_code metadataError;
            if (!readMetadata(entryPath, metadata, metadataError)) {
                result.traversal_complete = false;
                skip(result, entryPath, SkipReason::inaccessible_entry, metadataError);
            } else if (metadata.is_reparse_point) {
                // Windows junctions, mount points, and symbolic links are all reparse points.
                skip(result, entryPath, SkipReason::reparse_point);
            } else if (metadata.is_directory) {
                pendingDirectories.push_back(entryPath);
            } else if (!metadata.is_regular_file) {
                skip(result, entryPath, SkipReason::unsupported_file_type);
            } else {
                std::error_code sizeError;
                const std::uintmax_t fileSize = std::filesystem::file_size(entryPath, sizeError);
                if (sizeError) {
                    result.traversal_complete = false;
                    skip(result, entryPath, SkipReason::file_size_error, sizeError);
                } else if (fileSize > std::numeric_limits<std::uintmax_t>::max()
                                         - result.total_logical_bytes) {
                    result.traversal_complete = false;
                    skip(result, entryPath, SkipReason::file_size_error,
                         std::make_error_code(std::errc::value_too_large));
                } else {
                    ScannedFile scannedFile;
                    scannedFile.path = entryPath;
                    scannedFile.logical_size_bytes = fileSize;
                    scannedFile.attributes = metadata.attributes;

                    std::uintmax_t allocatedSize = 0;
                    std::error_code allocationError;
                    if (getAllocatedSize(entryPath, allocatedSize, allocationError)) {
                        scannedFile.allocated_size_bytes = allocatedSize;
                        scannedFile.allocated_size_available = true;
                        if (allocatedSize > std::numeric_limits<std::uintmax_t>::max()
                                                - result.total_allocated_bytes) {
                            result.allocation_measurements_complete = false;
                            result.allocation_measurement_failures.push_back(
                                {entryPath,
                                 std::make_error_code(std::errc::value_too_large)});
                        } else {
                            result.total_allocated_bytes += allocatedSize;
                        }
                    } else {
                        result.allocation_measurements_complete = false;
                        result.allocation_measurement_failures.push_back(
                            {entryPath, allocationError});
                        OperationError issue;
                        issue.code = classifyWindowsError(
                            static_cast<unsigned long>(allocationError.value()));
                        issue.path = entryPath;
                        issue.native_code = allocationError.value();
                        issue.technical_details = allocationError.message();
                        result.errors.push_back(std::move(issue));
                    }
                    scannedFile.compression_state = inspectFileCompressionState(
                        entryPath, metadata.attributes.compressed,
                        !scannedFile.allocated_size_available
                            || allocatedSize < fileSize);

                    result.files.push_back(std::move(scannedFile));
                    result.total_logical_bytes += fileSize;
                }
            }

            std::error_code incrementError;
            iterator.increment(incrementError);
            if (incrementError) {
                result.traversal_complete = false;
                skip(result, directory, SkipReason::traversal_error, incrementError);
                break;
            }
            if ((entriesVisited & 0x7fU) == 0U) {
                reportProgress();
            }
        }
        if (cancellation.stop_requested()) {
            result.cancelled = true;
            result.traversal_complete = false;
            break;
        }
    }

    if (cancellation.stop_requested()) {
        result.cancelled = true;
        result.traversal_complete = false;
    }

    result.total_file_count = result.files.size();
    reportProgress();
    return result;
}
