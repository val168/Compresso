#pragma once

#include <filesystem>
#include <string>

namespace compresso
{

enum class OperationErrorCode
{
    missing_directory,
    unsafe_target,
    insufficient_permissions,
    unsupported_filesystem,
    inaccessible_file,
    insufficient_disk_space,
    file_in_use,
    process_launch_failed,
    process_failed,
    process_terminated,
    file_changed,
    verification_failed,
    unexpected_failure
};

struct OperationError
{
    OperationErrorCode code = OperationErrorCode::unexpected_failure;
    std::filesystem::path path;
    int native_code = 0;
    int process_exit_code = -1;
    std::string technical_details;
};

[[nodiscard]] std::string operationErrorMessage(OperationErrorCode code);
[[nodiscard]] OperationErrorCode classifyWindowsError(unsigned long code);
[[nodiscard]] std::string operationErrorCodeName(OperationErrorCode code);

} // namespace compresso
