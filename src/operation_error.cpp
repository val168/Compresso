#include "compresso/operation_error.h"

#include <Windows.h>

std::string compresso::operationErrorMessage(OperationErrorCode code)
{
    switch (code) {
    case OperationErrorCode::missing_directory:
        return "The selected directory no longer exists. Select it again.";
    case OperationErrorCode::unsafe_target:
        return "This location cannot be changed. Choose a game folder.";
    case OperationErrorCode::insufficient_permissions:
        return "Access is denied. Check the folder and file permissions.";
    case OperationErrorCode::unsupported_filesystem:
        return "Choose a local NTFS directory that supports LZX compression.";
    case OperationErrorCode::inaccessible_file:
        return "Some files could not be accessed. Close or unlock them and retry.";
    case OperationErrorCode::insufficient_disk_space:
        return "There is not enough free disk space to complete the operation.";
    case OperationErrorCode::file_in_use:
        return "A file is in use. Close applications using it and retry.";
    case OperationErrorCode::process_launch_failed:
        return "Windows compact.exe could not start. Check Windows system files and permissions.";
    case OperationErrorCode::process_failed:
        return "Windows could not complete LZX. Check free space, permissions, and file access.";
    case OperationErrorCode::process_terminated:
        return "Windows compact.exe stopped unexpectedly. Check the file and retry.";
    case OperationErrorCode::file_changed:
        return "A file changed during the operation. Close applications using it and retry.";
    case OperationErrorCode::verification_failed:
        return "The final compression state could not be verified. A follow-up scan is needed.";
    case OperationErrorCode::unexpected_failure:
        return "The operation could not be completed. See Details for diagnostics.";
    }
    return "The operation could not be completed. See Details for diagnostics.";
}

compresso::OperationErrorCode compresso::classifyWindowsError(unsigned long code)
{
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_DIRECTORY:
        return OperationErrorCode::missing_directory;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return OperationErrorCode::insufficient_permissions;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return OperationErrorCode::file_in_use;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return OperationErrorCode::insufficient_disk_space;
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION:
    case ERROR_UNRECOGNIZED_VOLUME:
        return OperationErrorCode::unsupported_filesystem;
    default:
        return OperationErrorCode::inaccessible_file;
    }
}

std::string compresso::operationErrorCodeName(OperationErrorCode code)
{
    switch (code) {
    case OperationErrorCode::missing_directory: return "missing_directory";
    case OperationErrorCode::unsafe_target: return "unsafe_target";
    case OperationErrorCode::insufficient_permissions: return "insufficient_permissions";
    case OperationErrorCode::unsupported_filesystem: return "unsupported_filesystem";
    case OperationErrorCode::inaccessible_file: return "inaccessible_file";
    case OperationErrorCode::insufficient_disk_space: return "insufficient_disk_space";
    case OperationErrorCode::file_in_use: return "file_in_use";
    case OperationErrorCode::process_launch_failed: return "process_launch_failed";
    case OperationErrorCode::process_failed: return "process_failed";
    case OperationErrorCode::process_terminated: return "process_terminated";
    case OperationErrorCode::file_changed: return "file_changed";
    case OperationErrorCode::verification_failed: return "verification_failed";
    case OperationErrorCode::unexpected_failure: return "unexpected_failure";
    }
    return "unexpected_failure";
}
