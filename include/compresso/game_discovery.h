#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace compresso
{

struct GameInstallation
{
    QString id;
    QString name;
    QString install_path;
    QString launcher;
    bool available = false;
};

class SteamDiscovery final
{
public:
    [[nodiscard]] static QList<GameInstallation> discover();
    [[nodiscard]] static QList<GameInstallation> parseLibrary(const QString& libraryPath);
    [[nodiscard]] static QStringList parseLibraryFolders(const QString& vdfText);
};

} // namespace compresso

Q_DECLARE_METATYPE(compresso::GameInstallation)
Q_DECLARE_METATYPE(QList<compresso::GameInstallation>)
