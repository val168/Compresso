#include "compresso/directory_validator.h"

#include <QFileInfo>

QString compresso::directoryValidationError(const QString& path)
{
    if (path.isEmpty()) {
        return QStringLiteral("Enter a game directory.");
    }

    const QFileInfo directory(path);
    if (!directory.exists()) {
        return QStringLiteral("Directory does not exist.");
    }
    if (!directory.isDir()) {
        return QStringLiteral("Path is not a directory.");
    }
    if (!directory.isReadable()) {
        return QStringLiteral("Directory is not accessible.");
    }

    return {};
}
