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
}

} // namespace

compresso::ScanResult compresso::DirectoryScanner::scan(const std::filesystem::path& root)
{
    ScanResult result;
    result.root = root;

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
    while (!pendingDirectories.empty()) {
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

        while (iterator != end) {
            const std::filesystem::path entryPath = iterator->path();
            EntryMetadata metadata;
            std::error_code metadataError;
            if (!readMetadata(entryPath, metadata, metadataError)) {
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
                    skip(result, entryPath, SkipReason::file_size_error, sizeError);
                } else if (fileSize > std::numeric_limits<std::uintmax_t>::max()
                                         - result.total_directory_size_bytes) {
                    skip(result, entryPath, SkipReason::file_size_error,
                         std::make_error_code(std::errc::value_too_large));
                } else {
                    result.files.push_back({entryPath, fileSize, metadata.attributes});
                    result.total_directory_size_bytes += fileSize;
                }
            }

            std::error_code incrementError;
            iterator.increment(incrementError);
            if (incrementError) {
                result.traversal_complete = false;
                skip(result, directory, SkipReason::traversal_error, incrementError);
                break;
            }
        }
    }

    result.total_file_count = result.files.size();
    return result;
}
