#include "compresso/game_discovery.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>

#include <algorithm>
#include <utility>

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
    // Steam's libraryfolders.vdf lists game libraries on other drives. Find the
    // client through its registry entries and standard install locations instead
    // of probing every mounted drive (which can stall on sleeping removable disks).
    static const QStringList roots = [] {
        QStringList candidates;
        const auto addCandidate = [&candidates](QString path) {
            path = QDir::fromNativeSeparators(path.trimmed());
            if (!path.isEmpty()) candidates.push_back(std::move(path));
        };

        QSettings currentUser(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"),
                              QSettings::NativeFormat);
        addCandidate(currentUser.value(QStringLiteral("SteamPath")).toString());

#ifdef _WIN32
        for (const QString& key : {
                 QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Valve\\Steam"),
                 QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Valve\\Steam")}) {
            QSettings machine(key, QSettings::NativeFormat);
            addCandidate(machine.value(QStringLiteral("InstallPath")).toString());
            addCandidate(machine.value(QStringLiteral("SteamPath")).toString());
        }
#endif

        for (const QString& parent : {qEnvironmentVariable("ProgramFiles(x86)"),
                                      qEnvironmentVariable("ProgramFiles"),
                                      qEnvironmentVariable("LOCALAPPDATA")}) {
            if (!parent.isEmpty()) addCandidate(QDir(parent).filePath(QStringLiteral("Steam")));
        }

        QStringList existing;
        QSet<QString> seen;
        for (const QString& candidate : std::as_const(candidates)) {
            const QString normalized = QDir::cleanPath(QFileInfo(candidate).absoluteFilePath());
            const QString key = normalized.toCaseFolded();
            if (seen.contains(key) || !QDir(normalized).exists()) continue;
            seen.insert(key);
            existing.push_back(normalized);
        }
        return existing;
    }();
    return roots;
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
    QSet<QString> seenLibraries;
    const auto addLibrary = [&libraries, &seenLibraries](const QString& path) {
        const QString normalized = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        const QString key = normalized.toCaseFolded();
        if (!seenLibraries.contains(key)) {
            seenLibraries.insert(key);
            libraries.push_back(normalized);
        }
    };
    for (const QString& root : steamRoots()) {
        addLibrary(root);
        QFile manifest(QDir(root).filePath(QStringLiteral("steamapps/libraryfolders.vdf")));
        if (!manifest.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QString text = QString::fromUtf8(manifest.readAll());
        for (const QString& library : parseLibraryFolders(text)) addLibrary(library);
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

QStringList compresso::SteamDiscovery::artworkCacheFiles(const QString& appId)
{
    static const QRegularExpression validAppId(QStringLiteral("^\\d+$"));
    if (!validAppId.match(appId).hasMatch()) return {};

    QStringList result;
    for (const QString& root : steamRoots()) {
        const QString cachePath = QDir(root).filePath(
            QStringLiteral("appcache/librarycache/%1").arg(appId));
        const QDir cache(cachePath);
        if (!cache.exists()) continue;
        QDirIterator files(cachePath, {QStringLiteral("*.jpg"), QStringLiteral("*.png")},
                           QDir::Files, QDirIterator::Subdirectories);
        QStringList candidates;
        while (files.hasNext()) candidates.push_back(files.next());
        const auto priority = [](const QString& path) {
            const QString name = QFileInfo(path).fileName().toCaseFolded();
            if (name == QStringLiteral("library_header.jpg")) return 0;
            if (name == QStringLiteral("header.jpg")) return 1;
            if (name == QStringLiteral("library_capsule.jpg")) return 2;
            return 3;
        };
        std::stable_sort(candidates.begin(), candidates.end(),
                         [&priority](const QString& left, const QString& right) {
            return priority(left) < priority(right);
        });
        result.append(candidates);
    }
    return result;
}
