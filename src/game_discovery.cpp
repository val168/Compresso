#include "compresso/game_discovery.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>

#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace
{

QString unescapeVdf(QString value)
{
    value.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
    value.replace(QStringLiteral("\\\""), QStringLiteral("\""));
    return value;
}

QString vdfValue(const QString& text, const QString& key)
{
    const QRegularExpression expression(
        QStringLiteral("\\\"%1\\\"\\s*\\\"((?:\\\\.|[^\\\"\\\\])*)\\\"")
            .arg(QRegularExpression::escape(key)),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = expression.match(text);
    return match.hasMatch() ? unescapeVdf(match.captured(1)) : QString{};
}

QString canonicalPath(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}

QStringList steamRoots()
{
    QStringList roots;
    QSettings registry(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"),
                       QSettings::NativeFormat);
    const QString registered = registry.value(QStringLiteral("SteamPath")).toString();
    if (!registered.isEmpty()) roots.push_back(QDir::fromNativeSeparators(registered));

#ifdef _WIN32
    const DWORD mask = GetLogicalDrives();
    for (int drive = 0; drive < 26; ++drive) {
        if ((mask & (1u << drive)) == 0) continue;
        wchar_t root[] = {static_cast<wchar_t>(L'A' + drive), L':', L'\\', L'\0'};
        const UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        const QString volume = QString::fromWCharArray(root);
        for (const QString& relative : {
                 QStringLiteral("Program Files (x86)/Steam"),
                 QStringLiteral("Program Files/Steam"), QStringLiteral("Steam"),
                 QStringLiteral("Games/Steam")}) {
            roots.push_back(QDir(volume).filePath(relative));
        }
    }
#endif

    QStringList existing;
    QSet<QString> seen;
    for (const QString& root : roots) {
        const QString normalized = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
        const QString key = normalized.toCaseFolded();
        if (QDir(normalized).exists() && !seen.contains(key)) {
            seen.insert(key);
            existing.push_back(normalized);
        }
    }
    return existing;
}

} // namespace

QList<compresso::GameInstallation> compresso::SteamDiscovery::parseLibrary(
    const QString& libraryPath)
{
    QList<GameInstallation> games;
    const QDir library(libraryPath);
    const QDir steamApps(library.filePath(QStringLiteral("steamapps")));
    if (!steamApps.exists()) return games;

    QDirIterator manifests(steamApps.path(), {QStringLiteral("appmanifest_*.acf")},
                           QDir::Files, QDirIterator::NoIteratorFlags);
    while (manifests.hasNext()) {
        QFile manifest(manifests.next());
        if (!manifest.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QString text = QString::fromUtf8(manifest.readAll());
        const QString appId = vdfValue(text, QStringLiteral("appid"));
        const QString name = vdfValue(text, QStringLiteral("name"));
        const QString installDir = vdfValue(text, QStringLiteral("installdir"));
        if (appId.isEmpty() || name.isEmpty() || installDir.isEmpty()) continue;
        const QString commonPath = QFileInfo(steamApps.filePath(QStringLiteral("common")))
                                       .absoluteFilePath();
        const QString path = QDir::cleanPath(QDir(commonPath).filePath(installDir));
        const QString relative = QDir(commonPath).relativeFilePath(path);
        if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")))
            continue;
        games.push_back({appId, name, QDir::cleanPath(path), QStringLiteral("Steam"),
                         QDir(path).exists()});
    }
    std::sort(games.begin(), games.end(), [](const GameInstallation& left,
                                              const GameInstallation& right) {
        return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
    });
    return games;
}

QStringList compresso::SteamDiscovery::parseLibraryFolders(const QString& vdfText)
{
    QStringList paths;
    QSet<QString> seen;
    const auto addPath = [&paths, &seen](QString value) {
        value = QDir::cleanPath(QDir::fromNativeSeparators(unescapeVdf(std::move(value))));
        const QString key = value.toCaseFolded();
        if (!value.isEmpty() && !seen.contains(key)) {
            seen.insert(key);
            paths.push_back(std::move(value));
        }
    };
    const QRegularExpression pathExpression(
        QStringLiteral("\\\"path\\\"\\s*\\\"((?:\\\\.|[^\\\"\\\\])*)\\\""),
        QRegularExpression::CaseInsensitiveOption);
    auto iterator = pathExpression.globalMatch(vdfText);
    while (iterator.hasNext()) addPath(iterator.next().captured(1));

    const QRegularExpression legacyPathExpression(
        QStringLiteral("\\\"\\d+\\\"\\s*\\\"((?:\\\\.|[^\\\"\\\\])*)\\\""));
    auto legacyPaths = legacyPathExpression.globalMatch(vdfText);
    while (legacyPaths.hasNext()) addPath(legacyPaths.next().captured(1));
    return paths;
}

QList<compresso::GameInstallation> compresso::SteamDiscovery::discover()
{
    QStringList libraries;
    for (const QString& root : steamRoots()) {
        libraries.push_back(root);
        QFile manifest(QDir(root).filePath(QStringLiteral("steamapps/libraryfolders.vdf")));
        if (!manifest.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QString text = QString::fromUtf8(manifest.readAll());
        libraries.append(parseLibraryFolders(text));
    }

    QList<GameInstallation> result;
    QSet<QString> seenPaths;
    for (const QString& library : libraries) {
        for (GameInstallation game : parseLibrary(library)) {
            const QString key = canonicalPath(game.install_path);
            if (seenPaths.contains(key)) continue;
            seenPaths.insert(key);
            result.push_back(std::move(game));
        }
    }
    std::sort(result.begin(), result.end(), [](const GameInstallation& left,
                                                const GameInstallation& right) {
        return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
    });
    return result;
}
