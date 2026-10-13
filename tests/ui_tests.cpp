#include "compresso/main_window.h"
#include "compresso/directory_validator.h"
#include "compresso/directory_scanner.h"
#include "compresso/compression_backend.h"
#include "compresso/game_discovery.h"

#include <QDir>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QProgressBar>
#include <QProcess>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QScreen>
#include <QTableView>
#include <QSettings>
#include <QSize>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include <Windows.h>
#include <winioctl.h>

namespace
{

struct MountPointReparseBuffer
{
    DWORD tag;
    WORD dataLength;
    WORD reserved;
    WORD substituteNameOffset;
    WORD substituteNameLength;
    WORD printNameOffset;
    WORD printNameLength;
    WCHAR pathBuffer[1024];
};

bool createDirectoryJunction(const std::filesystem::path& junction,
                             const std::filesystem::path& target)
{
    if (!CreateDirectoryW(junction.c_str(), nullptr)) {
        return false;
    }

    HANDLE handle = CreateFileW(junction.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(junction.c_str());
        return false;
    }

    const std::wstring substituteName = L"\\??\\" + target.native();
    const std::size_t substituteBytes = substituteName.size() * sizeof(WCHAR);
    const std::size_t pathBytes = substituteBytes + 2 * sizeof(WCHAR);
    MountPointReparseBuffer buffer{};
    buffer.tag = IO_REPARSE_TAG_MOUNT_POINT;
    buffer.dataLength = static_cast<WORD>(8 + pathBytes);
    buffer.substituteNameLength = static_cast<WORD>(substituteBytes);
    buffer.printNameOffset = static_cast<WORD>(substituteBytes + sizeof(WCHAR));
    std::copy(substituteName.begin(), substituteName.end(), buffer.pathBuffer);

    DWORD bytesReturned = 0;
    const DWORD inputSize = static_cast<DWORD>(
        offsetof(MountPointReparseBuffer, pathBuffer) + pathBytes);
    const BOOL created = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &buffer, inputSize,
                                         nullptr, 0, &bytesReturned, nullptr);
    CloseHandle(handle);
    if (!created) {
        RemoveDirectoryW(junction.c_str());
        return false;
    }
    return true;
}

QString compressionReportDetails(const compresso::CompressionReport& report)
{
    QStringList details;
    if (!report.rejection_reason.isEmpty()) {
        details.push_back(report.rejection_reason);
    }
    for (const auto& file : report.files) {
        QString entry = QString::fromStdWString(file.path.native())
                        + QStringLiteral(" [state %1, exit %2]")
                              .arg(static_cast<int>(file.state))
                              .arg(file.exit_code);
        if (!file.error_message.isEmpty()) {
            entry += QStringLiteral("\n") + file.error_message;
        }
        if (file.error && !file.error->technical_details.empty()) {
            entry += QStringLiteral("\n")
                     + QString::fromUtf8(file.error->technical_details.c_str());
        }
        if (!file.standard_output.isEmpty()) {
            entry += QStringLiteral("\nstdout: ")
                     + QString::fromLocal8Bit(file.standard_output);
        }
        if (!file.standard_error.isEmpty()) {
            entry += QStringLiteral("\nstderr: ")
                     + QString::fromLocal8Bit(file.standard_error);
        }
        details.push_back(std::move(entry));
    }
    return details.join(QLatin1Char('\n'));
}

class ScopedWindowsEnvironmentVariable final
{
public:
    ScopedWindowsEnvironmentVariable(const wchar_t* name, const std::wstring& value)
        : name_(name)
    {
        const DWORD required = GetEnvironmentVariableW(name_.c_str(), nullptr, 0);
        existed_ = required > 0;
        if (existed_) {
            std::wstring previous(required, L'\0');
            const DWORD length = GetEnvironmentVariableW(name_.c_str(), previous.data(), required);
            if (length < required) {
                previous.resize(length);
                previous_ = std::move(previous);
            } else {
                existed_ = false;
            }
        }
        SetEnvironmentVariableW(name_.c_str(), value.c_str());
    }

    ~ScopedWindowsEnvironmentVariable()
    {
        SetEnvironmentVariableW(name_.c_str(), existed_ ? previous_.c_str() : nullptr);
    }

    ScopedWindowsEnvironmentVariable(const ScopedWindowsEnvironmentVariable&) = delete;
    ScopedWindowsEnvironmentVariable& operator=(const ScopedWindowsEnvironmentVariable&) = delete;

private:
    std::wstring name_;
    std::wstring previous_;
    bool existed_ = false;
};

} // namespace

class MainWindowTests final : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("CompressoTests"));
        QCoreApplication::setApplicationName(QStringLiteral("CompressoTests"));
        QSettings settings;
        settings.remove(QStringLiteral("directory/lastSelected"));
        settings.remove(QStringLiteral("window/geometry"));
    }

    void cleanup()
    {
        QSettings settings;
        settings.remove(QStringLiteral("directory/lastSelected"));
        settings.remove(QStringLiteral("window/geometry"));
        settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("system"));
    }

    void presentsDirectoryAndCompressionDetails()
    {
        MainWindow window;

        QCOMPARE(window.windowTitle(), QStringLiteral("Compresso"));

        const auto* directoryPath = window.findChild<QLineEdit*>("gameDirectoryPath");
        QVERIFY(directoryPath);
        QCOMPARE(directoryPath->placeholderText(), QStringLiteral("Select a game folder"));

        const auto* originalSize = window.findChild<QLabel*>("originalSizeValue");
        const auto* compressedSize = window.findChild<QLabel*>("compressedSizeValue");
        const auto* method = window.findChild<QLabel*>("compressionMethodValue");
        const auto* progress = window.findChild<QProgressBar*>("operationProgress");
        QVERIFY(originalSize);
        QVERIFY(compressedSize);
        QVERIFY(method);
        QVERIFY(progress);
        QVERIFY(progress->isHidden());
        QCOMPARE(originalSize->text(), QStringLiteral("—"));
        QCOMPARE(compressedSize->text(), QStringLiteral("—"));
        QCOMPARE(method->text(), QStringLiteral("LZX"));
    }

    void navigationContainsOnlyLibraryAndQueue()
    {
        MainWindow window;

        QVERIFY(window.findChild<QPushButton*>("libraryNav"));
        QVERIFY(window.findChild<QPushButton*>("queueNav"));
        QVERIFY(!window.findChild<QPushButton*>("settingsNav"));
    }

    void parsesSteamLibraryManifestsWithoutInventingEntries()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QString steamApps = QDir(temporaryDirectory.path()).filePath("steamapps");
        const QString installedPath = QDir(steamApps).filePath("common/Installed");
        QVERIFY(QDir().mkpath(installedPath));

        QFile installedManifest(QDir(steamApps).filePath("appmanifest_480.acf"));
        QVERIFY(installedManifest.open(QIODevice::WriteOnly | QIODevice::Text));
        installedManifest.write(R"("AppState" { "appid" "480" "name" "Installed Game" "installdir" "Installed" })");
        installedManifest.close();

        QFile missingManifest(QDir(steamApps).filePath("appmanifest_481.acf"));
        QVERIFY(missingManifest.open(QIODevice::WriteOnly | QIODevice::Text));
        missingManifest.write(R"("AppState" { "appid" "481" "name" "Missing Game" "installdir" "Missing" })");
        missingManifest.close();

        QFile malformedManifest(QDir(steamApps).filePath("appmanifest_482.acf"));
        QVERIFY(malformedManifest.open(QIODevice::WriteOnly | QIODevice::Text));
        malformedManifest.write(R"("AppState" { "appid" "482" "name" "No Install Directory" })");
        malformedManifest.close();

        const auto games = compresso::SteamDiscovery::parseLibrary(temporaryDirectory.path());
        QCOMPARE(games.size(), 2);
        QCOMPARE(games[0].id, QStringLiteral("480"));
        QCOMPARE(games[0].name, QStringLiteral("Installed Game"));
        QVERIFY(games[0].available);
        QCOMPARE(games[1].id, QStringLiteral("481"));
        QVERIFY(!games[1].available);
    }

    void parsesNewAndLegacySteamLibraryFolders()
    {
        const QString newFormat = QStringLiteral(
            "\"libraryfolders\" { \"0\" { \"path\" \"C:\\\\Steam\" } "
            "\"1\" { \"path\" \"D:\\\\Games\\\\SteamLibrary\" } }");
        const QStringList newPaths = compresso::SteamDiscovery::parseLibraryFolders(newFormat);
        QCOMPARE(newPaths.size(), 2);
        QVERIFY(newPaths.contains(QStringLiteral("C:/Steam")));
        QVERIFY(newPaths.contains(QStringLiteral("D:/Games/SteamLibrary")));

        const QString oldFormat = QStringLiteral(
            "\"LibraryFolders\" { \"0\" \"C:\\\\Steam\" "
            "\"1\" \"E:\\\\Games\\\\SteamLibrary\" }");
        const QStringList oldPaths = compresso::SteamDiscovery::parseLibraryFolders(oldFormat);
        QCOMPARE(oldPaths.size(), 2);
        QVERIFY(oldPaths.contains(QStringLiteral("C:/Steam")));
        QVERIFY(oldPaths.contains(QStringLiteral("E:/Games/SteamLibrary")));
    }

    void supportsManualLightAndDarkThemes()
    {
        MainWindow window;
        auto* light = window.findChild<QAction*>("themeLightAction");
        auto* dark = window.findChild<QAction*>("themeDarkAction");
        auto* system = window.findChild<QAction*>("themeSystemAction");
        auto* path = window.findChild<QLineEdit*>("gameDirectoryPath");
        auto* progress = window.findChild<QProgressBar*>("operationProgress");
        QVERIFY(light);
        QVERIFY(dark);
        QVERIFY(system);
        QVERIFY(path);
        QVERIFY(progress);

        dark->trigger();
        QVERIFY(dark->isChecked());
        QVERIFY(qApp->palette().color(QPalette::Base).lightness()
                < qApp->palette().color(QPalette::Text).lightness());
        QCOMPARE(path->palette().color(QPalette::Base),
                 qApp->palette().color(QPalette::Base));
        QCOMPARE(progress->palette().color(QPalette::Window),
                 qApp->palette().color(QPalette::Window));

        light->trigger();
        QVERIFY(light->isChecked());
        QVERIFY(qApp->palette().color(QPalette::Base).lightness()
                > qApp->palette().color(QPalette::Text).lightness());
        system->trigger();
        QVERIFY(system->isChecked());
    }

    void selectedGameArtworkHasFallbackWhenNoGameIsSelected()
    {
        MainWindow window;
        const auto* artwork = window.findChild<QLabel*>("selectedGameArtwork");
        QVERIFY(artwork);
        QVERIFY(!artwork->pixmap().isNull());
    }

    void widgetsRespectHighDpiScale()
    {
        MainWindow window;
        QVERIFY(window.devicePixelRatioF() >= 1.0);
        const QByteArray configuredScale = qgetenv("QT_SCALE_FACTOR");
        if (!configuredScale.isEmpty()) {
            const qreal expectedScale = configuredScale.toDouble();
            QVERIFY(qAbs(window.devicePixelRatioF() - expectedScale) < 0.01);
        }
        if (qApp->primaryScreen() != nullptr) {
            QVERIFY(qApp->primaryScreen()->devicePixelRatio() >= 1.0);
        }
    }

    void restoresValidatedDirectoryAndWindowGeometry()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QByteArray savedGeometry;
        QSize expectedSize;
        {
            MainWindow source;
            source.resize(660, 410);
            savedGeometry = source.saveGeometry();
            expectedSize = source.size();
        }

        QSettings settings;
        settings.setValue(QStringLiteral("directory/lastSelected"), directory.path());
        settings.setValue(QStringLiteral("window/geometry"), savedGeometry);
        settings.sync();
        QCOMPARE(compresso::directoryValidationError(directory.path()), QString());
        QCOMPARE(settings.value(QStringLiteral("directory/lastSelected")).toString(),
                 directory.path());

        MainWindow restored;
        const auto* path = restored.findChild<QLineEdit*>("gameDirectoryPath");
        const auto* analyze = restored.findChild<QPushButton*>("analyzeButton");
        QVERIFY(path);
        QVERIFY(analyze);
        QCOMPARE(path->text(), directory.path());
        QVERIFY(analyze->isEnabled());
        // At elevated scale factors Qt may clamp the restored width to the available
        // virtual desktop. Height remains within bounds and confirms that geometry loaded.
        QCOMPARE(restored.height(), expectedSize.height());
    }

    void ignoresInvalidSavedDirectoryAndMalformedSettings()
    {
        QSettings settings;
        settings.setValue(QStringLiteral("directory/lastSelected"),
                          QDir::tempPath() + QStringLiteral("/Compresso-missing-directory"));
        settings.setValue(QStringLiteral("window/geometry"), QStringLiteral("invalid geometry"));
        settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("obsolete-theme"));

        MainWindow window;
        const auto* path = window.findChild<QLineEdit*>("gameDirectoryPath");
        const auto* analyze = window.findChild<QPushButton*>("analyzeButton");
        const auto* systemTheme = window.findChild<QAction*>("themeSystemAction");
        QVERIFY(path);
        QVERIFY(analyze);
        QVERIFY(systemTheme);
        QVERIFY(path->text().isEmpty());
        QVERIFY(!analyze->isEnabled());
        QVERIFY(systemTheme->isChecked());
        QVERIFY(window.minimumWidth() >= 420);
        QVERIFY(!settings.contains(QStringLiteral("directory/lastSelected")));
    }

    void savesValidatedDirectoryAndWindowGeometry()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        MainWindow window;
        window.resize(640, 390);
        auto* path = window.findChild<QLineEdit*>("gameDirectoryPath");
        QVERIFY(path);
        window.show();
        path->setText(directory.path());
        QVERIFY(QMetaObject::invokeMethod(path, "editingFinished", Qt::DirectConnection));

        QSettings settings;
        QCOMPARE(settings.value(QStringLiteral("directory/lastSelected")).toString(),
                 directory.path());
        window.close();
        QVERIFY(!settings.value(QStringLiteral("window/geometry")).toByteArray().isEmpty());
    }

    void enablesAnalysisOnlyAfterDirectorySelection()
    {
        MainWindow window;

        const auto* browse = window.findChild<QPushButton*>("browseButton");
        QVERIFY(browse);
        QVERIFY(browse->isEnabled());

        const auto* analyze = window.findChild<QPushButton*>("analyzeButton");
        QVERIFY(analyze);
        QVERIFY(!analyze->isEnabled());

        for (const auto* name : {"compressButton", "restoreButton"}) {
            const auto* button = window.findChild<QPushButton*>(name);
            QVERIFY(button);
            QVERIFY(!button->isEnabled());
        }

        const auto* status = window.findChild<QStatusBar*>("operationStatus");
        QVERIFY(status);
        QVERIFY(status->currentMessage().contains(QStringLiteral("Select a game directory")));
    }

    void validatesDirectoryPaths()
    {
        QCOMPARE(compresso::directoryValidationError({}),
                 QStringLiteral("Enter a game directory."));
        QCOMPARE(compresso::directoryValidationError(
                     QStringLiteral("Z:/Compresso/nonexistent directory")),
                 QStringLiteral("Directory does not exist."));

        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QString filePath = QDir(temporaryDirectory.path()).filePath("not-a-directory.txt");
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QCOMPARE(compresso::directoryValidationError(filePath),
                 QStringLiteral("Path is not a directory."));
    }

    void acceptsManuallyEnteredUnicodePathAndClearsOldAnalysis()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());

        const QString directoryName = QString::fromUtf8("Games with spaces \xE2\x98\x95");
        const QString gameDirectory = QDir(temporaryDirectory.path()).filePath(directoryName);
        QVERIFY(QDir().mkpath(gameDirectory));

        MainWindow window;
        auto* pathField = window.findChild<QLineEdit*>("gameDirectoryPath");
        auto* originalSize = window.findChild<QLabel*>("originalSizeValue");
        auto* compressedSize = window.findChild<QLabel*>("compressedSizeValue");
        auto* status = window.findChild<QStatusBar*>("operationStatus");
        QVERIFY(pathField);
        QVERIFY(originalSize);
        QVERIFY(compressedSize);
        QVERIFY(status);

        pathField->setText(gameDirectory);
        QCOMPARE(status->currentMessage(),
                 QStringLiteral("Directory changed. Press Enter to validate."));
        pathField->setFocus();
        QTest::keyClick(pathField, Qt::Key_Return);
        QCOMPARE(pathField->text(), gameDirectory);
        QCOMPARE(status->currentMessage(),
                 QStringLiteral("Directory is ready. Analyze it before compression."));

        originalSize->setText("Previously analyzed");
        compressedSize->setText("Previously analyzed");
        pathField->setText(temporaryDirectory.path());
        QCOMPARE(originalSize->text(), QStringLiteral("—"));
        QCOMPARE(compressedSize->text(), QStringLiteral("—"));
        QCOMPARE(status->currentMessage(),
                 QStringLiteral("Directory changed. Press Enter to validate."));
    }

    void reportsManualPathValidationErrors()
    {
        MainWindow window;
        auto* pathField = window.findChild<QLineEdit*>("gameDirectoryPath");
        auto* status = window.findChild<QStatusBar*>("operationStatus");
        QVERIFY(pathField);
        QVERIFY(status);

        pathField->setText(QStringLiteral("Z:/Compresso/nonexistent directory"));
        pathField->setFocus();
        QTest::keyClick(pathField, Qt::Key_Return);
        QCOMPARE(status->currentMessage(), QStringLiteral("Directory does not exist."));
    }

    void recursivelyScansFilesAndTotalsLogicalSizes()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());

        const QString gamePath = QDir(temporaryDirectory.path()).filePath(
            QString::fromUtf8("Game install \xE2\x98\x95"));
        const QString nestedPath = QDir(gamePath).filePath(
            QString::fromUtf8("Data with spaces/Textures"));
        QVERIFY(QDir().mkpath(nestedPath));

        const std::filesystem::path gameDirectory(gamePath.toStdWString());
        const std::filesystem::path firstFile = gameDirectory / L"readme.txt";
        const std::filesystem::path secondFile =
            std::filesystem::path(nestedPath.toStdWString()) / L"asset.bin";
        {
            std::ofstream file(firstFile, std::ios::binary);
            QVERIFY(file.is_open());
            file << "12345";
        }
        {
            std::ofstream file(secondFile, std::ios::binary);
            QVERIFY(file.is_open());
            file << "abcdefgh";
        }
        QVERIFY(SetFileAttributesW(firstFile.c_str(),
                                   FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN));

        const compresso::ScanResult result = compresso::DirectoryScanner::scan(gameDirectory);
        QVERIFY(result.traversal_complete);
        QCOMPARE(result.total_file_count, std::size_t{2});
        QCOMPARE(result.total_logical_bytes, std::uintmax_t{13});
        QVERIFY(result.allocation_measurements_complete);
        QCOMPARE(result.allocation_measurement_failures.size(), std::size_t{0});
        std::uintmax_t sumOfFileAllocations = 0;
        for (const auto& file : result.files) {
            QVERIFY(file.allocated_size_available);
            sumOfFileAllocations += file.allocated_size_bytes;
        }
        QCOMPARE(result.total_allocated_bytes, sumOfFileAllocations);
        QVERIFY(!result.volume.error);
        QVERIFY(!result.volume.filesystem_type.empty());
        QCOMPARE(result.volume.compression_eligible,
                 result.volume.filesystem_type == L"NTFS"
                     && result.volume.supports_file_compression);
        QVERIFY(result.skipped_entries.empty());
        QVERIFY(std::any_of(result.files.begin(), result.files.end(), [&](const auto& file) {
            return file.path == firstFile && file.logical_size_bytes == 5;
        }));
        QVERIFY(std::any_of(result.files.begin(), result.files.end(), [&](const auto& file) {
            return file.path == secondFile && file.logical_size_bytes == 8;
        }));
        const auto attributedFile = std::find_if(result.files.begin(), result.files.end(),
                                                  [&](const auto& file) {
                                                      return file.path == firstFile;
                                                  });
        QVERIFY(attributedFile != result.files.end());
        QVERIFY(attributedFile->attributes.read_only);
        QVERIFY(attributedFile->attributes.hidden);
        QVERIFY(SetFileAttributesW(firstFile.c_str(), FILE_ATTRIBUTE_NORMAL));
    }

    void scansEmptyDirectoryWithZeroSizes()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());

        const auto root = std::filesystem::path(temporaryDirectory.path().toStdWString());
        const compresso::ScanResult result = compresso::DirectoryScanner::scan(root);
        QVERIFY(result.traversal_complete);
        QVERIFY(result.allocation_measurements_complete);
        QCOMPARE(result.total_file_count, std::size_t{0});
        QCOMPARE(result.total_logical_bytes, std::uintmax_t{0});
        QCOMPARE(result.total_allocated_bytes, std::uintmax_t{0});
        QVERIFY(!result.volume.error);
    }

    void compressionSizeSummaryArithmetic()
    {
        const auto saving = compresso::calculateCompressionSizeSummary(1000, 250);
        QCOMPARE(saving.bytes_saved, std::uintmax_t{750});
        QCOMPARE(saving.bytes_increased, std::uintmax_t{0});
        QVERIFY(saving.percentage_available);
        QCOMPARE(static_cast<double>(saving.percentage_saved), 75.0);

        const auto unchanged = compresso::calculateCompressionSizeSummary(1000, 1000);
        QCOMPARE(unchanged.bytes_saved, std::uintmax_t{0});
        QCOMPARE(unchanged.bytes_increased, std::uintmax_t{0});
        QCOMPARE(static_cast<double>(unchanged.percentage_saved), 0.0);

        const auto increased = compresso::calculateCompressionSizeSummary(1000, 1250);
        QCOMPARE(increased.bytes_saved, std::uintmax_t{0});
        QCOMPARE(increased.bytes_increased, std::uintmax_t{250});
        QVERIFY(increased.percentage_available);
        QCOMPARE(static_cast<double>(increased.percentage_saved), -25.0);

        const auto empty = compresso::calculateCompressionSizeSummary(0, 0);
        QCOMPARE(empty.bytes_saved, std::uintmax_t{0});
        QVERIFY(empty.percentage_available);
        QCOMPARE(static_cast<double>(empty.percentage_saved), 0.0);

        const auto grewFromEmpty = compresso::calculateCompressionSizeSummary(0, 256);
        QCOMPARE(grewFromEmpty.bytes_increased, std::uintmax_t{256});
        QVERIFY(!grewFromEmpty.percentage_available);
    }

    void scannerReportsTraversalProgress()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const auto root = std::filesystem::path(temporaryDirectory.path().toStdWString());
        for (int index = 0; index < 260; ++index) {
            std::ofstream file(root / (L"entry-" + std::to_wstring(index) + L".dat"),
                               std::ios::binary);
            QVERIFY(file.is_open());
            file << "x";
        }

        std::size_t callbacks = 0;
        const auto result = compresso::DirectoryScanner::scan(
            root, [&callbacks](std::size_t, std::size_t) { ++callbacks; });
        QCOMPARE(result.total_file_count, std::size_t{260});
        QVERIFY(callbacks >= std::size_t{3});
    }

    void scannerCancellationReturnsPartialAndMarksItIncomplete()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const auto root = std::filesystem::path(temporaryDirectory.path().toStdWString());
        for (int index = 0; index < 512; ++index) {
            std::ofstream file(root / (L"cancel-scan-" + std::to_wstring(index) + L".dat"),
                               std::ios::binary);
            QVERIFY(file.is_open());
            file << "data";
        }

        std::stop_source cancellation;
        const auto result = compresso::DirectoryScanner::scan(
            root, [&cancellation](std::size_t entries, std::size_t) {
                if (entries >= 128) cancellation.request_stop();
            }, cancellation.get_token());
        QVERIFY(result.cancelled);
        QVERIFY(!result.traversal_complete);
        QVERIFY(result.total_file_count > 0);
        QVERIFY(result.total_file_count < 512);
    }

    void reportsRootFilesystemFailures()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const auto missingDirectory = std::filesystem::path(
            QDir(temporaryDirectory.path()).filePath("missing").toStdWString());

        const compresso::ScanResult result = compresso::DirectoryScanner::scan(missingDirectory);
        QVERIFY(!result.traversal_complete);
        QCOMPARE(result.total_file_count, std::size_t{0});
        QCOMPARE(result.skipped_entries.size(), std::size_t{1});
        QCOMPARE(result.skipped_entries.front().reason,
                 compresso::SkipReason::inaccessible_directory);
        QVERIFY(static_cast<bool>(result.skipped_entries.front().error));
    }

    void skipsDirectoryAndFileSymlinksWithoutReadingOutsideFiles()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());

        const QString basePath = temporaryDirectory.path();
        const QString gamePath = QDir(basePath).filePath("game");
        const QString outsidePath = QDir(basePath).filePath("outside");
        QVERIFY(QDir().mkpath(gamePath));
        QVERIFY(QDir().mkpath(outsidePath));

        const auto gameDirectory = std::filesystem::path(gamePath.toStdWString());
        const auto outsideDirectory = std::filesystem::path(outsidePath.toStdWString());
        const auto outsideFile = outsideDirectory / L"outside.dat";
        {
            std::ofstream file(outsideFile, std::ios::binary);
            QVERIFY(file.is_open());
            file << "outside content";
        }
        {
            std::ofstream file(gameDirectory / L"inside.dat", std::ios::binary);
            QVERIFY(file.is_open());
            file << "inside";
        }

        const QString junctionPath = QDir(gamePath).filePath("outside junction");
        const auto junction = std::filesystem::path(junctionPath.toStdWString());
        const bool junctionCreated = createDirectoryJunction(junction, outsideDirectory);

        const QString symlinkPath = QDir(gamePath).filePath("outside symlink");
        std::error_code symlinkError;
        std::filesystem::create_directory_symlink(
            outsideDirectory, std::filesystem::path(symlinkPath.toStdWString()), symlinkError);
        if (!junctionCreated && symlinkError) {
            QSKIP("This Windows environment does not permit creating a test directory symlink.");
        }

        const QString fileSymlinkPath = QDir(gamePath).filePath("outside file symlink.dat");
        std::error_code fileSymlinkError;
        std::filesystem::create_symlink(
            outsideFile, std::filesystem::path(fileSymlinkPath.toStdWString()), fileSymlinkError);

        const compresso::ScanResult result = compresso::DirectoryScanner::scan(gameDirectory);
        QVERIFY(result.traversal_complete);
        QCOMPARE(result.total_file_count, std::size_t{1});
        QCOMPARE(result.total_logical_bytes, std::uintmax_t{6});
        QVERIFY(!result.skipped_entries.empty());
        QVERIFY(std::any_of(result.skipped_entries.begin(), result.skipped_entries.end(),
                            [](const auto& entry) {
                                return entry.reason == compresso::SkipReason::reparse_point;
                            }));
        if (junctionCreated) {
            QVERIFY(std::any_of(result.skipped_entries.begin(), result.skipped_entries.end(),
                                [&](const auto& entry) { return entry.path == junction; }));
            const compresso::ScanResult rootJunctionResult =
                compresso::DirectoryScanner::scan(junction);
            QCOMPARE(rootJunctionResult.total_file_count, std::size_t{0});
            QCOMPARE(rootJunctionResult.skipped_entries.size(), std::size_t{1});
            QCOMPARE(rootJunctionResult.skipped_entries.front().reason,
                     compresso::SkipReason::reparse_point);
            RemoveDirectoryW(junction.c_str());
        }

        if (!fileSymlinkError) {
            QVERIFY(std::any_of(result.skipped_entries.begin(), result.skipped_entries.end(),
                                [&](const auto& entry) {
                                    return entry.path
                                           == std::filesystem::path(fileSymlinkPath.toStdWString());
                                }));
        }
    }

    void analyzeDisplaysEssentialScanResults()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QString filePath = QDir(temporaryDirectory.path()).filePath("game data.bin");
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("123456789"), qint64{9});
        file.close();

        MainWindow window;
        auto* pathField = window.findChild<QLineEdit*>("gameDirectoryPath");
        auto* analyze = window.findChild<QPushButton*>("analyzeButton");
        auto* originalSize = window.findChild<QLabel*>("originalSizeValue");
        auto* fileCount = window.findChild<QLabel*>("fileCountValue");
        auto* compressedSize = window.findChild<QLabel*>("compressedSizeValue");
        auto* compress = window.findChild<QPushButton*>("compressButton");
        auto* progress = window.findChild<QProgressBar*>("operationProgress");
        auto* status = window.findChild<QStatusBar*>("operationStatus");
        QVERIFY(pathField);
        QVERIFY(analyze);
        QVERIFY(originalSize);
        QVERIFY(fileCount);
        QVERIFY(compressedSize);
        QVERIFY(compress);
        QVERIFY(progress);
        QVERIFY(status);

        pathField->setText(temporaryDirectory.path());
        pathField->setFocus();
        QTest::keyClick(pathField, Qt::Key_Return);
        QVERIFY(analyze->isEnabled());
        QTest::mouseClick(analyze, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(status->currentMessage().startsWith("Scan complete:"), 5000);

        QCOMPARE(originalSize->text(), QStringLiteral("9 bytes"));
        QCOMPARE(fileCount->text(), QStringLiteral("1"));
        const auto scanned = compresso::DirectoryScanner::scan(
            std::filesystem::path(temporaryDirectory.path().toStdWString()));
        QCOMPARE(compressedSize->text(),
                 QLocale().toString(static_cast<qulonglong>(scanned.total_allocated_bytes))
                     + " bytes");
        QVERIFY(window.findChild<QLabel*>("filesystemValue"));
        const auto scan = compresso::DirectoryScanner::scan(
            std::filesystem::path(temporaryDirectory.path().toStdWString()));
        QCOMPARE(compress->isEnabled(), scan.volume.compression_eligible
                                            && scan.total_file_count > 0);
        auto* restore = window.findChild<QPushButton*>("restoreButton");
        QVERIFY(restore);
        QCOMPARE(restore->isEnabled(), scan.volume.compression_eligible
                                           && scan.total_file_count > 0);
        if (restore->isEnabled()) {
            const QString scanStatus = status->currentMessage();
            QString confirmationText;
            bool defaultsToNo = false;
            QTimer::singleShot(0, [&confirmationText, &defaultsToNo] {
                auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                if (confirmation) {
                    confirmationText = confirmation->text();
                    defaultsToNo = confirmation->defaultButton()
                                   == confirmation->button(QMessageBox::No);
                    confirmation->button(QMessageBox::No)->click();
                }
            });
            QTest::mouseClick(restore, Qt::LeftButton);
            QVERIFY(confirmationText.contains("transparent LZX compression"));
            QVERIFY(defaultsToNo);
            QCOMPARE(status->currentMessage(), scanStatus);
            QVERIFY(progress->isHidden());
        }
        if (compress->isEnabled()) {
            auto* browse = window.findChild<QPushButton*>("browseButton");
            QVERIFY(browse);
            QTest::mouseClick(compress, Qt::LeftButton);
            QVERIFY(!progress->isHidden());
            QVERIFY(!pathField->isEnabled());
            QVERIFY(!browse->isEnabled());
            QVERIFY(!analyze->isEnabled());
            QVERIFY(!compress->isEnabled());
            QTRY_VERIFY_WITH_TIMEOUT(status->currentMessage().startsWith("LZX:"), 5000);
            QTRY_COMPARE_WITH_TIMEOUT(progress->maximum(), 1000, 5000);
            QVERIFY(!compress->isEnabled());
            QTRY_VERIFY_WITH_TIMEOUT(analyze->isEnabled(), 5000);
            QVERIFY(progress->isHidden());
        }
    }

    void scanProblemsHaveAnOptionalDetailsView()
    {
        QTemporaryDir temporaryDirectory;
        QTemporaryDir outsideDirectory;
        QVERIFY(temporaryDirectory.isValid());
        QVERIFY(outsideDirectory.isValid());
        const auto junction = std::filesystem::path(temporaryDirectory.path().toStdWString())
                              / L"outside game data";
        const auto outside = std::filesystem::path(outsideDirectory.path().toStdWString());
        if (!createDirectoryJunction(junction, outside)) {
            QSKIP("Directory junctions are not available in this test environment.");
        }

        MainWindow window;
        auto* pathField = window.findChild<QLineEdit*>("gameDirectoryPath");
        auto* analyze = window.findChild<QPushButton*>("analyzeButton");
        auto* detailsButton = window.findChild<QPushButton*>("operationDetailsButton");
        QVERIFY(pathField);
        QVERIFY(analyze);
        QVERIFY(detailsButton);
        QVERIFY(detailsButton->isHidden());
        pathField->setText(temporaryDirectory.path());
        pathField->setFocus();
        QTest::keyClick(pathField, Qt::Key_Return);
        QTest::mouseClick(analyze, Qt::LeftButton);
        auto* status = window.findChild<QStatusBar*>("operationStatus");
        QVERIFY(status);
        QTRY_VERIFY_WITH_TIMEOUT(status->currentMessage().startsWith("Scan complete:"), 5000);
        QVERIFY(!detailsButton->isHidden());
        QVERIFY(status->currentMessage().contains("See Details"));

        QTest::mouseClick(detailsButton, Qt::LeftButton);
        auto* log = window.findChild<QPlainTextEdit*>("operationDiagnosticLog");
        QVERIFY(log);
        QVERIFY(log->isReadOnly());
        QVERIFY(log->toPlainText().contains("symbolic link or junction"));
        window.findChild<QDialog*>()->close();
        RemoveDirectoryW(junction.c_str());
    }

    void asynchronousScanKeepsUiResponsiveAndLocksConflictingControls()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const auto root = std::filesystem::path(temporaryDirectory.path().toStdWString());
        for (int index = 0; index < 700; ++index) {
            std::ofstream file(root / (L"game-file-" + std::to_wstring(index) + L".bin"),
                               std::ios::binary);
            QVERIFY(file.is_open());
            file << "game data";
        }

        MainWindow window;
        auto* pathField = window.findChild<QLineEdit*>("gameDirectoryPath");
        auto* analyze = window.findChild<QPushButton*>("analyzeButton");
        auto* browse = window.findChild<QPushButton*>("browseButton");
        auto* compress = window.findChild<QPushButton*>("compressButton");
        auto* progress = window.findChild<QProgressBar*>("operationProgress");
        auto* status = window.findChild<QStatusBar*>("operationStatus");
        QVERIFY(pathField);
        QVERIFY(analyze);
        QVERIFY(browse);
        QVERIFY(compress);
        QVERIFY(progress);
        QVERIFY(status);

        pathField->setText(temporaryDirectory.path());
        pathField->setFocus();
        QTest::keyClick(pathField, Qt::Key_Return);
        QVERIFY(analyze->isEnabled());

        int eventLoopTicks = 0;
        QTimer timer;
        timer.setInterval(1);
        connect(&timer, &QTimer::timeout, this, [&eventLoopTicks] { ++eventLoopTicks; });
        timer.start();
        QTest::mouseClick(analyze, Qt::LeftButton);
        QVERIFY(!progress->isHidden());
        QCOMPARE(progress->minimum(), 0);
        QCOMPARE(progress->maximum(), 0);
        QVERIFY(!pathField->isEnabled());
        QVERIFY(!browse->isEnabled());
        QVERIFY(!analyze->isEnabled());
        QVERIFY(!compress->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(status->currentMessage().startsWith("Scan complete:"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(analyze->isEnabled(), 5000);
        timer.stop();

        QVERIFY(eventLoopTicks > 0);
        QVERIFY(progress->isHidden());
        QCOMPARE(pathField->isEnabled(), true);
        QCOMPARE(browse->isEnabled(), true);
        QCOMPARE(analyze->isEnabled(), true);
        const auto scan = compresso::DirectoryScanner::scan(root);
        QCOMPARE(compress->isEnabled(), scan.volume.compression_eligible
                                            && scan.total_file_count > 0);
    }

    void cancelButtonStopsAnActiveScan()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const auto root = std::filesystem::path(temporaryDirectory.path().toStdWString());
        for (int index = 0; index < 2048; ++index) {
            std::ofstream file(root / (L"ui-cancel-" + std::to_wstring(index) + L".dat"),
                               std::ios::binary);
            QVERIFY(file.is_open());
            file << "file data";
        }

        MainWindow window;
        auto* pathField = window.findChild<QLineEdit*>("gameDirectoryPath");
        auto* analyze = window.findChild<QPushButton*>("analyzeButton");
        auto* cancel = window.findChild<QPushButton*>("cancelButton");
        auto* status = window.findChild<QStatusBar*>("operationStatus");
        QVERIFY(pathField);
        QVERIFY(analyze);
        QVERIFY(cancel);
        QVERIFY(status);
        QVERIFY(cancel->isHidden());

        pathField->setText(temporaryDirectory.path());
        QTest::keyClick(pathField, Qt::Key_Return);
        QTest::mouseClick(analyze, Qt::LeftButton);
        QVERIFY(!cancel->isHidden());
        QVERIFY(cancel->isEnabled());
        QTest::mouseClick(cancel, Qt::LeftButton);
        QTest::mouseClick(cancel, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(status->currentMessage().startsWith("Scan cancelled"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(cancel->isHidden(), 5000);
    }

    void closingDuringAnOperationWaitsForWorkerCleanup()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const auto root = std::filesystem::path(temporaryDirectory.path().toStdWString());
        for (int index = 0; index < 2000; ++index) {
            std::ofstream file(root / (L"shutdown-file-" + std::to_wstring(index) + L".bin"),
                               std::ios::binary);
            QVERIFY(file.is_open());
            file << "data";
        }

        MainWindow window;
        window.show();
        auto* pathField = window.findChild<QLineEdit*>("gameDirectoryPath");
        auto* analyze = window.findChild<QPushButton*>("analyzeButton");
        auto* status = window.findChild<QStatusBar*>("operationStatus");
        QVERIFY(pathField);
        QVERIFY(analyze);
        QVERIFY(status);
        pathField->setText(temporaryDirectory.path());
        pathField->setFocus();
        QTest::keyClick(pathField, Qt::Key_Return);
        QTest::mouseClick(analyze, Qt::LeftButton);
        QVERIFY(!pathField->isEnabled());

        window.close();
        QVERIFY(window.isVisible());
        QCOMPARE(status->currentMessage(),
                 QStringLiteral("Cancelling the current operation before closing\u2026"));
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 10000);
    }

    void compressionIntegrationUsesCompactAndPreservesFileContents()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());

        const QString gameDirectory = QDir(temporaryDirectory.path()).filePath(
            QString::fromUtf8("Game files \xE2\x98\x95"));
        QVERIFY(QDir().mkpath(gameDirectory));
        const QString filePath = QDir(gameDirectory).filePath("large asset with spaces.bin");
        const QByteArray contents(1024 * 1024, '\0');
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), static_cast<qint64>(contents.size()));
        file.close();

        const auto beforeScan = compresso::DirectoryScanner::scan(
            std::filesystem::path(gameDirectory.toStdWString()));
        QVERIFY(beforeScan.allocation_measurements_complete);
        QVERIFY(beforeScan.traversal_complete);
        QCOMPARE(beforeScan.files.size(), std::size_t{1});
        QCOMPARE(beforeScan.files.front().compression_state,
                 compresso::FileCompressionState::uncompressed);
        if (!beforeScan.volume.compression_eligible) {
            QSKIP("Temporary directory is not on a supported local NTFS volume.");
        }

        std::vector<compresso::CompressionProgress> progressUpdates;
        const auto report = compresso::CompressionBackend::compressDirectory(
            std::filesystem::path(gameDirectory.toStdWString()),
            [&progressUpdates](const auto& progress) { progressUpdates.push_back(progress); });
        if (!report.target_eligible) {
            QSKIP(qPrintable("Temporary directory is not on a supported local NTFS volume: "
                             + report.rejection_reason));
        }

        QCOMPARE(report.files.size(), std::size_t{1});
        QVERIFY2(report.compressedCount() == 1,
                 qPrintable(compressionReportDetails(report)));
        QCOMPARE(report.failedCount(), std::size_t{0});
        QCOMPARE(report.skippedCount(), std::size_t{0});
        QVERIFY(report.size_summary.has_value());
        QCOMPARE(report.size_summary->original_allocated_bytes,
                 beforeScan.total_allocated_bytes);
        QCOMPARE(report.files.front().exit_code, 0);
        const auto compressedScan = compresso::DirectoryScanner::scan(
            std::filesystem::path(gameDirectory.toStdWString()));
        QCOMPARE(compressedScan.files.size(), std::size_t{1});
        QCOMPARE(compressedScan.files.front().compression_state,
                 compresso::FileCompressionState::lzx_compressed);
        QCOMPARE(compresso::inspectFileCompressionState(
                     std::filesystem::path(filePath.toStdWString()), false, false),
                 compresso::FileCompressionState::lzx_compressed);
        QVERIFY(!report.files.front().standard_output.isEmpty()
                || !report.files.front().standard_error.isEmpty());
        QVERIFY(progressUpdates.size() >= std::size_t{3});
        QVERIFY(std::any_of(progressUpdates.begin(), progressUpdates.end(), [](const auto& update) {
            return update.totals_known && update.completed_files == 0
                   && update.total_files == 1;
        }));
        QCOMPARE(progressUpdates.back().completed_files, std::size_t{1});
        QCOMPARE(progressUpdates.back().total_files, std::size_t{1});
        QCOMPARE(progressUpdates.back().processed_logical_bytes,
                 static_cast<std::uintmax_t>(contents.size()));
        QCOMPARE(progressUpdates.back().total_logical_bytes,
                 static_cast<std::uintmax_t>(contents.size()));
        QVERIFY(progressUpdates.back().elapsed.count() >= 0);
        QVERIFY(std::any_of(progressUpdates.begin(), progressUpdates.end(), [](const auto& update) {
            return update.current_file.filename() == L"large asset with spaces.bin";
        }));

        QFile afterCompression(filePath);
        QVERIFY(afterCompression.open(QIODevice::ReadOnly));
        QCOMPARE(afterCompression.readAll(), contents);
        afterCompression.close();
        DWORD allocatedHigh = 0;
        SetLastError(ERROR_SUCCESS);
        const DWORD allocatedLow = GetCompressedFileSizeW(
            std::filesystem::path(filePath.toStdWString()).c_str(), &allocatedHigh);
        QVERIFY(allocatedLow != INVALID_FILE_SIZE || GetLastError() == NO_ERROR);
        const std::uintmax_t allocatedBytes =
            (static_cast<std::uintmax_t>(allocatedHigh) << 32) | allocatedLow;
        QCOMPARE(report.size_summary->final_allocated_bytes, allocatedBytes);
        QVERIFY(allocatedBytes < static_cast<std::uintmax_t>(contents.size()));

        const QString outsideAlias = QDir(temporaryDirectory.path()).filePath("outside hard link.bin");
        QVERIFY(CreateHardLinkW(std::filesystem::path(outsideAlias.toStdWString()).c_str(),
                                std::filesystem::path(filePath.toStdWString()).c_str(), nullptr));
        bool processStartedForHardLink = false;
        const auto hardLinkReport = compresso::CompressionBackend::restoreDirectory(
            std::filesystem::path(gameDirectory.toStdWString()), {},
            [&processStartedForHardLink](const auto&, const auto&, std::stop_token) {
                processStartedForHardLink = true;
                return compresso::CompactProcessResult{};
            });
        QCOMPARE(hardLinkReport.skippedCount(), std::size_t{1});
        QCOMPARE(hardLinkReport.restoredCount(), std::size_t{0});
        QVERIFY(!processStartedForHardLink);
        QVERIFY(DeleteFileW(std::filesystem::path(outsideAlias.toStdWString()).c_str()));

        const auto failedRestore = compresso::CompressionBackend::restoreDirectory(
            std::filesystem::path(gameDirectory.toStdWString()), {},
            [](const std::filesystem::path&, const std::filesystem::path&, std::stop_token) {
                compresso::CompactProcessResult result;
                result.started = true;
                result.finished = true;
                result.exit_code = 1;
                result.standard_error = "simulated restoration failure";
                return result;
            });
        QCOMPARE(failedRestore.failedCount(), std::size_t{1});
        QCOMPARE(failedRestore.restoredCount(), std::size_t{0});
        QCOMPARE(failedRestore.files.front().standard_error,
                 QByteArray("simulated restoration failure"));

        const auto restoreReport = compresso::CompressionBackend::restoreDirectory(
            std::filesystem::path(gameDirectory.toStdWString()));
        const QString restoreFailure = restoreReport.files.empty()
            ? QStringLiteral("No per-file result was returned.")
            : restoreReport.files.front().error_message + "\n"
                  + QString::fromUtf8(restoreReport.files.front().error
                      ? restoreReport.files.front().error->technical_details.c_str() : "");
        QVERIFY2(restoreReport.restoredCount() == 1, qPrintable(restoreFailure));
        QCOMPARE(restoreReport.failedCount(), std::size_t{0});
        QCOMPARE(restoreReport.skippedCount(), std::size_t{0});
        QCOMPARE(restoreReport.files.front().exit_code, 0);
        const auto restoredScan = compresso::DirectoryScanner::scan(
            std::filesystem::path(gameDirectory.toStdWString()));
        QCOMPARE(restoredScan.files.size(), std::size_t{1});
        QCOMPARE(restoredScan.files.front().compression_state,
                 compresso::FileCompressionState::uncompressed);
        QFile afterRestoration(filePath);
        QVERIFY(afterRestoration.open(QIODevice::ReadOnly));
        QCOMPARE(afterRestoration.readAll(), contents);
        afterRestoration.close();
        std::uintmax_t restoredAllocatedBytes = 0;
        DWORD restoredHigh = 0;
        SetLastError(ERROR_SUCCESS);
        const DWORD restoredLow = GetCompressedFileSizeW(
            std::filesystem::path(filePath.toStdWString()).c_str(), &restoredHigh);
        QVERIFY(restoredLow != INVALID_FILE_SIZE || GetLastError() == NO_ERROR);
        restoredAllocatedBytes = (static_cast<std::uintmax_t>(restoredHigh) << 32)
                                 | restoredLow;
        QVERIFY(restoredAllocatedBytes >= allocatedBytes);
    }

    void compressionUiShowsMeasuredAllocationSummary()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QString gameDirectory = QDir(temporaryDirectory.path()).filePath("Measured game");
        QVERIFY(QDir().mkpath(gameDirectory));
        const QString filePath = QDir(gameDirectory).filePath("asset.bin");
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray contents(256 * 1024, '\0');
        QCOMPARE(file.write(contents), static_cast<qint64>(contents.size()));
        file.close();

        MainWindow window;
        window.show();
        auto* pathField = window.findChild<QLineEdit*>("gameDirectoryPath");
        auto* analyze = window.findChild<QPushButton*>("analyzeButton");
        auto* compress = window.findChild<QPushButton*>("compressButton");
        auto* status = window.findChild<QStatusBar*>("operationStatus");
        auto* originalValue = window.findChild<QLabel*>("originalSizeValue");
        auto* finalValue = window.findChild<QLabel*>("compressedSizeValue");
        auto* selectedStatus = window.findChild<QLabel*>("selectedStatus");
        auto* gameTable = window.findChild<QTableView*>("gameTable");
        QVERIFY(pathField);
        QVERIFY(analyze);
        QVERIFY(compress);
        QVERIFY(status);
        QVERIFY(originalValue);
        QVERIFY(finalValue);
        QVERIFY(selectedStatus);
        QVERIFY(gameTable);

        pathField->setText(gameDirectory);
        QTest::keyClick(pathField, Qt::Key_Return);
        QTest::mouseClick(analyze, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(compress->isEnabled()
                                     || status->currentMessage().contains("LZX unavailable"),
                                 10000);
        if (!compress->isEnabled()) {
            QSKIP("Temporary directory is not on a supported local NTFS volume.");
        }
        QCOMPARE(selectedStatus->text(), QStringLiteral("Uncompressed"));

        const auto beforeScan = compresso::DirectoryScanner::scan(
            std::filesystem::path(gameDirectory.toStdWString()));
        QVERIFY(beforeScan.traversal_complete);
        QVERIFY(beforeScan.allocation_measurements_complete);
        QTest::mouseClick(compress, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(selectedStatus->text(), QStringLiteral("Compressed"), 30000);

        const auto afterScan = compresso::DirectoryScanner::scan(
            std::filesystem::path(gameDirectory.toStdWString()));
        QVERIFY(afterScan.traversal_complete);
        QVERIFY(afterScan.allocation_measurements_complete);
        QCOMPARE(originalValue->text(),
                 QLocale().toString(static_cast<qulonglong>(beforeScan.total_allocated_bytes))
                     + " bytes");
        QCOMPARE(finalValue->text(),
                 QLocale().toString(static_cast<qulonglong>(afterScan.total_allocated_bytes))
                     + " bytes");
        QTRY_VERIFY_WITH_TIMEOUT(status->currentMessage().startsWith("Scan complete:"), 20000);
        QCOMPARE(selectedStatus->text(), QStringLiteral("Compressed"));
        QCOMPARE(gameTable->model()->index(0, 6).data().toString(),
                 QStringLiteral("Compressed"));
        QCOMPARE(gameTable->model()->index(0, 7).data().toString(), QStringLiteral("Restore"));
    }

    void restorationSkipsNonLzxFiles()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QString gameDirectory = QDir(temporaryDirectory.path()).filePath("uncompressed game");
        QVERIFY(QDir().mkpath(gameDirectory));
        const QString filePath = QDir(gameDirectory).filePath("ordinary file.bin");
        const QByteArray contents("ordinary uncompressed content");
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), static_cast<qint64>(contents.size()));
        file.close();

        bool processStarted = false;
        const auto report = compresso::CompressionBackend::restoreDirectory(
            std::filesystem::path(gameDirectory.toStdWString()), {},
            [&processStarted](const auto&, const auto&, std::stop_token) {
                processStarted = true;
                return compresso::CompactProcessResult{};
            });
        if (!report.target_eligible) {
            QSKIP(qPrintable("Temporary directory is not on a supported local NTFS volume: "
                             + report.rejection_reason));
        }

        QCOMPARE(report.restoredCount(), std::size_t{0});
        QCOMPARE(report.skippedCount(), std::size_t{1});
        QCOMPARE(report.failedCount(), std::size_t{0});
        QVERIFY(!processStarted);
        QFile unchanged(filePath);
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCOMPARE(unchanged.readAll(), contents);
    }

    void compressionReportsFailedProcessAndCompletedProgress()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QString gameDirectory = QDir(temporaryDirectory.path()).filePath("failed game");
        QVERIFY(QDir().mkpath(gameDirectory));
        const QString filePath = QDir(gameDirectory).filePath("failing asset.bin");
        const QByteArray contents(4096, 'f');
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), static_cast<qint64>(contents.size()));
        file.close();

        std::vector<compresso::CompressionProgress> progressUpdates;
        const std::filesystem::path expectedFile(filePath.toStdWString());
        bool runnerReceivedExpectedArguments = false;
        const auto report = compresso::CompressionBackend::compressDirectory(
            std::filesystem::path(gameDirectory.toStdWString()),
            [&progressUpdates](const auto& progress) { progressUpdates.push_back(progress); },
            [&expectedFile, &runnerReceivedExpectedArguments](
                const std::filesystem::path& executable,
                const std::filesystem::path& fileToCompress, std::stop_token) {
                runnerReceivedExpectedArguments =
                    executable.filename() == std::filesystem::path(L"compact.exe")
                    && fileToCompress.filename() == expectedFile.filename();
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                compresso::CompactProcessResult result;
                result.started = true;
                result.finished = true;
                result.exit_code = 5;
                result.standard_error = "simulated compact failure";
                return result;
            });
        if (!report.target_eligible) {
            QSKIP(qPrintable("Temporary directory is not on a supported local NTFS volume: "
                             + report.rejection_reason));
        }

        QCOMPARE(report.failedCount(), std::size_t{1});
        QVERIFY(runnerReceivedExpectedArguments);
        QCOMPARE(report.skippedCount(), std::size_t{0});
        QCOMPARE(report.compressedCount(), std::size_t{0});
        QCOMPARE(report.files.front().exit_code, 5);
        QCOMPARE(report.files.front().standard_error, QByteArray("simulated compact failure"));
        QVERIFY(report.files.front().error);
        QCOMPARE(report.files.front().error->code, compresso::OperationErrorCode::process_failed);
        QVERIFY(QString::fromUtf8(report.files.front().error->technical_details.c_str())
                    .contains("simulated compact failure"));
        QCOMPARE(progressUpdates.back().completed_files, std::size_t{1});
        QCOMPARE(progressUpdates.back().processed_logical_bytes,
                 static_cast<std::uintmax_t>(contents.size()));
        QVERIFY(progressUpdates.back().elapsed >= std::chrono::milliseconds(30));
        QVERIFY(progressUpdates.back().current_file.empty());

        QFile unchanged(filePath);
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCOMPARE(unchanged.readAll(), contents);
    }

    void compressionSkipsFilesOpenForWriting()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QString gameDirectory = QDir(temporaryDirectory.path()).filePath("busy game");
        QVERIFY(QDir().mkpath(gameDirectory));
        const QString filePath = QDir(gameDirectory).filePath("active.bin");
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("active data"), qint64{11});
        file.close();

        HANDLE writer = CreateFileW(std::filesystem::path(filePath.toStdWString()).c_str(),
                                    GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(writer != INVALID_HANDLE_VALUE);

        std::vector<compresso::CompressionProgress> progressUpdates;
        const auto report = compresso::CompressionBackend::compressDirectory(
            std::filesystem::path(gameDirectory.toStdWString()),
            [&progressUpdates](const auto& progress) { progressUpdates.push_back(progress); });
        CloseHandle(writer);
        if (!report.target_eligible) {
            QSKIP(qPrintable("Temporary directory is not on a supported local NTFS volume: "
                             + report.rejection_reason));
        }

        QCOMPARE(report.compressedCount(), std::size_t{0});
        QCOMPARE(report.skippedCount() + report.failedCount(), std::size_t{1});
        QVERIFY(report.files.front().error);
        QCOMPARE(report.files.front().error->code,
                 compresso::OperationErrorCode::process_failed);
        QCOMPARE(progressUpdates.back().completed_files, std::size_t{1});
        QCOMPARE(progressUpdates.back().total_files, std::size_t{1});
        QCOMPARE(progressUpdates.back().processed_logical_bytes, std::uintmax_t{11});
        QVERIFY(progressUpdates.back().current_file.empty());
        QFile unchanged(filePath);
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCOMPARE(unchanged.readAll(), QByteArray("active data"));
    }

    void operationErrorsClassifyWindowsConditions()
    {
        QCOMPARE(compresso::classifyWindowsError(ERROR_ACCESS_DENIED),
                 compresso::OperationErrorCode::insufficient_permissions);
        QCOMPARE(compresso::classifyWindowsError(ERROR_SHARING_VIOLATION),
                 compresso::OperationErrorCode::file_in_use);
        QCOMPARE(compresso::classifyWindowsError(ERROR_DISK_FULL),
                 compresso::OperationErrorCode::insufficient_disk_space);
        QCOMPARE(compresso::classifyWindowsError(ERROR_NOT_SUPPORTED),
                 compresso::OperationErrorCode::unsupported_filesystem);
        QVERIFY(QString::fromStdString(compresso::operationErrorMessage(
                    compresso::OperationErrorCode::process_failed))
                    .contains("free space"));
    }

    void missingDirectoryIsReportedStructurally()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const auto missing = std::filesystem::path(temporaryDirectory.path().toStdWString())
                             / L"missing game folder";

        const auto scan = compresso::DirectoryScanner::scan(missing);
        QVERIFY(!scan.traversal_complete);
        QVERIFY(!scan.errors.empty());
        QCOMPARE(scan.errors.back().code, compresso::OperationErrorCode::missing_directory);

        const auto report = compresso::CompressionBackend::compressDirectory(missing);
        QVERIFY(!report.target_eligible);
        QVERIFY(report.error);
        QCOMPARE(report.error->code, compresso::OperationErrorCode::missing_directory);
    }

    void processLaunchFailureAndUnexpectedTerminationAreNotSuccess()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QString gameDirectory = QDir(temporaryDirectory.path()).filePath("process failure game");
        QVERIFY(QDir().mkpath(gameDirectory));
        for (const QString& name : {QStringLiteral("first.bin"), QStringLiteral("second.bin")}) {
            QFile file(QDir(gameDirectory).filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("test file"), qint64{9});
        }

        std::size_t launchAttempts = 0;
        const auto launchFailure = compresso::CompressionBackend::compressDirectory(
            std::filesystem::path(gameDirectory.toStdWString()), {},
            [&launchAttempts](const auto&, const auto&, std::stop_token) {
                ++launchAttempts;
                compresso::CompactProcessResult result;
                result.error_code = compresso::OperationErrorCode::process_launch_failed;
                result.error_message = "simulated failed start";
                return result;
            });
        if (!launchFailure.target_eligible) {
            QSKIP(qPrintable("Temporary directory is not on a supported local NTFS volume: "
                             + launchFailure.rejection_reason));
        }
        QCOMPARE(launchAttempts, std::size_t{1});
        QCOMPARE(launchFailure.compressedCount(), std::size_t{0});
        QCOMPARE(launchFailure.failedCount(), std::size_t{1});
        QCOMPARE(launchFailure.skippedCount(), std::size_t{1});
        QCOMPARE(launchFailure.files.front().error->code,
                 compresso::OperationErrorCode::process_launch_failed);
        QVERIFY(launchFailure.files.back().skip_reason);
        QCOMPARE(*launchFailure.files.back().skip_reason,
                 compresso::CompressionSkipReason::operation_aborted);

        std::size_t crashAttempts = 0;
        const auto crashed = compresso::CompressionBackend::compressDirectory(
            std::filesystem::path(gameDirectory.toStdWString()), {},
            [&crashAttempts](const auto&, const auto&, std::stop_token) {
                ++crashAttempts;
                compresso::CompactProcessResult result;
                result.started = true;
                result.finished = true;
                result.crashed = true;
                result.error_code = compresso::OperationErrorCode::process_terminated;
                result.error_message = "simulated crash";
                return result;
            });
        QCOMPARE(crashAttempts, std::size_t{1});
        QCOMPARE(crashed.compressedCount(), std::size_t{0});
        QCOMPARE(crashed.failedCount(), std::size_t{1});
        QCOMPARE(crashed.skippedCount(), std::size_t{1});
        QCOMPARE(crashed.files.front().error->code,
                 compresso::OperationErrorCode::process_terminated);
    }

    void diskSpaceFailureStopsFurtherCompressionAttempts()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QString gameDirectory = QDir(temporaryDirectory.path()).filePath("disk full game");
        QVERIFY(QDir().mkpath(gameDirectory));
        for (const QString& name : {QStringLiteral("asset one.bin"), QStringLiteral("asset two.bin")}) {
            QFile file(QDir(gameDirectory).filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("asset data"), qint64{10});
        }

        std::size_t attempts = 0;
        const auto report = compresso::CompressionBackend::compressDirectory(
            std::filesystem::path(gameDirectory.toStdWString()), {},
            [&attempts](const auto&, const auto&, std::stop_token) {
                ++attempts;
                compresso::CompactProcessResult result;
                result.started = true;
                result.finished = true;
                result.exit_code = ERROR_DISK_FULL;
                result.error_code = compresso::OperationErrorCode::insufficient_disk_space;
                result.standard_error = "simulated disk full";
                return result;
            });
        if (!report.target_eligible) {
            QSKIP(qPrintable("Temporary directory is not on a supported local NTFS volume: "
                             + report.rejection_reason));
        }
        QCOMPARE(attempts, std::size_t{1});
        QCOMPARE(report.compressedCount(), std::size_t{0});
        QCOMPARE(report.failedCount(), std::size_t{1});
        QCOMPARE(report.skippedCount(), std::size_t{1});
        QCOMPARE(report.files.front().error->code,
                 compresso::OperationErrorCode::insufficient_disk_space);
    }

    void compressionCancellationBeforeSchedulingDoesNothing()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QString gameDirectory = QDir(temporaryDirectory.path()).filePath("cancel before start");
        QVERIFY(QDir().mkpath(gameDirectory));
        QFile file(QDir(gameDirectory).filePath("asset.bin"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(4096, 'x')), qint64{4096});
        file.close();
        const auto root = std::filesystem::path(gameDirectory.toStdWString());
        if (!compresso::DirectoryScanner::scan(root).volume.compression_eligible) {
            QSKIP("Temporary directory is not on a supported local NTFS volume.");
        }

        std::stop_source cancellation;
        cancellation.request_stop();
        cancellation.request_stop();
        bool processStarted = false;
        const auto report = compresso::CompressionBackend::compressDirectory(
            root, {}, [&processStarted](const auto&, const auto&, std::stop_token) {
                processStarted = true;
                return compresso::CompactProcessResult{};
            }, cancellation.get_token());
        QVERIFY(report.cancelled);
        QVERIFY(!processStarted);
        QCOMPARE(report.compressedCount(), std::size_t{0});
        QVERIFY(!report.size_summary);
    }

    void activeProcessCancellationStopsSchedulingAndIsNotAFileFailure()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QString gameDirectory = QDir(temporaryDirectory.path()).filePath("cancel active");
        QVERIFY(QDir().mkpath(gameDirectory));
        for (const QString& name : {QStringLiteral("active.bin"), QStringLiteral("later.bin")}) {
            QFile file(QDir(gameDirectory).filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(QByteArray(64 * 1024, 'x')), qint64{64 * 1024});
        }
        const auto root = std::filesystem::path(gameDirectory.toStdWString());
        if (!compresso::DirectoryScanner::scan(root).volume.compression_eligible) {
            QSKIP("Temporary directory is not on a supported local NTFS volume.");
        }

        std::stop_source cancellation;
        std::atomic_bool processStarted = false;
        std::atomic_size_t attempts = 0;
        compresso::CompressionReport report;
        std::thread operation([&] {
            report = compresso::CompressionBackend::compressDirectory(
                root, {}, [&processStarted, &attempts](const auto&, const auto&,
                                                       std::stop_token token) {
                    ++attempts;
                    processStarted = true;
                    while (!token.stop_requested()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    compresso::CompactProcessResult result;
                    result.started = true;
                    result.cancelled = true;
                    result.error_message = "simulated process shutdown after cancellation";
                    return result;
                }, cancellation.get_token());
        });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!processStarted && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!processStarted) {
            cancellation.request_stop();
            operation.join();
            QFAIL("The first compact process was not scheduled.");
        }
        cancellation.request_stop();
        cancellation.request_stop();
        operation.join();

        QVERIFY(report.cancelled);
        QCOMPARE(attempts.load(), std::size_t{1});
        QCOMPARE(report.cancelledCount(), std::size_t{1});
        QCOMPARE(report.skippedCount(), std::size_t{1});
        QCOMPARE(report.failedCount(), std::size_t{0});
        QCOMPARE(report.compressedCount(), std::size_t{0});
        QVERIFY(!report.size_summary);
    }

    void cancellationAfterACompletedFilePreservesPartialCompression()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QString gameDirectory = QDir(temporaryDirectory.path()).filePath("partial compression");
        QVERIFY(QDir().mkpath(gameDirectory));
        for (const QString& name : {QStringLiteral("first asset.bin"),
                                    QStringLiteral("second asset.bin")}) {
            QFile file(QDir(gameDirectory).filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(QByteArray(1024 * 1024, '\0')), qint64{1024 * 1024});
        }
        const auto root = std::filesystem::path(gameDirectory.toStdWString());
        if (!compresso::DirectoryScanner::scan(root).volume.compression_eligible) {
            QSKIP("Temporary directory is not on a supported local NTFS volume.");
        }

        std::stop_source cancellation;
        const auto report = compresso::CompressionBackend::compressDirectory(
            root, [&cancellation](const auto& progress) {
                if (progress.totals_known && progress.completed_files == 1) {
                    cancellation.request_stop();
                }
            }, {}, cancellation.get_token());
        QVERIFY(report.cancelled);
        QVERIFY2(report.compressedCount() == 1,
                 qPrintable(compressionReportDetails(report)));
        QCOMPARE(report.skippedCount(), std::size_t{1});
        QCOMPARE(report.failedCount(), std::size_t{0});
        QVERIFY(!report.size_summary);

        const auto compressedFile = std::find_if(
            report.files.begin(), report.files.end(), [](const auto& result) {
                return result.state == compresso::CompressionFileState::compressed;
            });
        QVERIFY(compressedFile != report.files.end());
        QFile unchanged(QString::fromStdWString(compressedFile->path.native()));
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCOMPARE(unchanged.readAll(), QByteArray(1024 * 1024, '\0'));
    }

    void restorationCancellationLeavesTheActiveFileInPlace()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QString gameDirectory = QDir(temporaryDirectory.path()).filePath("cancel restore");
        QVERIFY(QDir().mkpath(gameDirectory));
        const QString filePath = QDir(gameDirectory).filePath("lzx asset.bin");
        const QByteArray contents(1024 * 1024, '\0');
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), static_cast<qint64>(contents.size()));
        file.close();
        const auto root = std::filesystem::path(gameDirectory.toStdWString());
        if (!compresso::DirectoryScanner::scan(root).volume.compression_eligible) {
            QSKIP("Temporary directory is not on a supported local NTFS volume.");
        }

        const auto compression = compresso::CompressionBackend::compressDirectory(root);
        QVERIFY(compression.target_eligible);
        QVERIFY2(compression.compressedCount() == 1,
                 qPrintable(compressionReportDetails(compression)));

        std::stop_source cancellation;
        const auto restoration = compresso::CompressionBackend::restoreDirectory(
            root, {}, [&cancellation](const auto&, const auto&, std::stop_token) {
                cancellation.request_stop();
                compresso::CompactProcessResult result;
                result.started = true;
                result.cancelled = true;
                return result;
            }, cancellation.get_token());
        QVERIFY(restoration.cancelled);
        QCOMPARE(restoration.cancelledCount(), std::size_t{1});
        QCOMPARE(restoration.restoredCount(), std::size_t{0});
        QCOMPARE(restoration.failedCount(), std::size_t{0});

        QFile stillAccessible(filePath);
        QVERIFY(stillAccessible.open(QIODevice::ReadOnly));
        QCOMPARE(stillAccessible.readAll(), contents);
    }

    void compressionRejectsVolumeRoots()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const auto tempPath = std::filesystem::path(temporaryDirectory.path().toStdWString());
        std::wstring volumeRoot(32768, L'\0');
        QVERIFY(GetVolumePathNameW(tempPath.c_str(), volumeRoot.data(),
                                   static_cast<DWORD>(volumeRoot.size())));
        volumeRoot.resize(std::char_traits<wchar_t>::length(volumeRoot.c_str()));

        const auto report = compresso::CompressionBackend::compressDirectory(
            std::filesystem::path(volumeRoot));
        QVERIFY(!report.target_eligible);
        QCOMPARE(report.rejection_reason, QStringLiteral("A volume root cannot be compressed."));
        QVERIFY(report.files.empty());
    }

    void compressionRejectsItsOwnInstallationDirectory()
    {
        const auto applicationDirectory = std::filesystem::path(
            QCoreApplication::applicationDirPath().toStdWString());
        const auto report =
            compresso::CompressionBackend::compressDirectory(applicationDirectory);
        QVERIFY(!report.target_eligible);
        QCOMPARE(report.rejection_reason,
                 QStringLiteral("Compresso's installation directory is excluded."));
        QVERIFY(report.files.empty());
    }

    void compressionRejectsWindowsDirectory()
    {
        std::wstring windowsDirectory(32768, L'\0');
        const UINT length = GetWindowsDirectoryW(
            windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
        QVERIFY(length > 0);
        QVERIFY(length < windowsDirectory.size());
        windowsDirectory.resize(length);

        const auto report = compresso::CompressionBackend::compressDirectory(
            std::filesystem::path(windowsDirectory));
        QVERIFY(!report.target_eligible);
        QCOMPARE(report.rejection_reason,
                 QStringLiteral("Windows and protected system locations are excluded."));
        QVERIFY(report.files.empty());
    }

    void compressionAllowsGameFoldersInProgramFilesButRejectsWindowsApps()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const std::filesystem::path programFiles =
            std::filesystem::path(temporaryDirectory.path().toStdWString())
            / L"Program Files (x86)";
        const std::filesystem::path gameDirectory =
            programFiles / L"Steam" / L"steamapps" / L"common" / L"Skyrim Special Edition";
        const std::filesystem::path windowsApps = programFiles / L"WindowsApps";
        std::error_code filesystemError;
        std::filesystem::create_directories(gameDirectory, filesystemError);
        QVERIFY(!filesystemError);
        std::filesystem::create_directories(windowsApps, filesystemError);
        QVERIFY(!filesystemError);
        {
            ScopedWindowsEnvironmentVariable programFilesOverride(
                L"ProgramFiles(x86)", programFiles.native());
            std::ofstream file(gameDirectory / L"game.bin", std::ios::binary);
            file << "disposable test data";
            file.close();

            std::size_t attempts = 0;
            const auto gameReport = compresso::CompressionBackend::compressDirectory(
                gameDirectory, {}, [&attempts](const auto&, const auto&, std::stop_token) {
                    ++attempts;
                    compresso::CompactProcessResult result;
                    result.started = true;
                    result.finished = true;
                    result.exit_code = 1;
                    result.error_message = "simulated failure";
                    return result;
                });
            if (!gameReport.target_eligible) {
                QSKIP(qPrintable("Temporary directory is not on a supported local NTFS volume: "
                                 + gameReport.rejection_reason));
            }
            QCOMPARE(attempts, std::size_t{1});

            const auto windowsAppsReport = compresso::CompressionBackend::compressDirectory(
                windowsApps);
            QVERIFY(!windowsAppsReport.target_eligible);
            QCOMPARE(windowsAppsReport.rejection_reason,
                     QStringLiteral("Windows and protected system locations are excluded."));
        }
    }
};

QTEST_MAIN(MainWindowTests)
#include "ui_tests.moc"
