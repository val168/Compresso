#pragma once

#include <QString>

namespace compresso
{

// Returns an empty string when path identifies an existing readable directory.
[[nodiscard]] QString directoryValidationError(const QString& path);

} // namespace compresso
