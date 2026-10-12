#include "compresso/main_window.h"
#include "compresso/directory_validator.h"
#include "compresso/directory_scanner.h"
#include "compresso/compression_backend.h"

#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QProgressBar>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <filesystem>
#include <fstream>
#include <algorithm>
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

} // namespace

class MainWindowTests final : public QObject
{
    Q_OBJECT

private slots:
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
                 QStringLiteral("Finishing the current operation before closing\u2026"));
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

        std::vector<compresso::CompressionProgress> progressUpdates;
        const auto report = compresso::CompressionBackend::compressDirectory(
            std::filesystem::path(gameDirectory.toStdWString()),
            [&progressUpdates](const auto& progress) { progressUpdates.push_back(progress); });
        if (!report.target_eligible) {
            QSKIP(qPrintable("Temporary directory is not on a supported local NTFS volume: "
                             + report.rejection_reason));
        }

        QCOMPARE(report.files.size(), std::size_t{1});
        QCOMPARE(report.compressedCount(), std::size_t{1});
        QCOMPARE(report.failedCount(), std::size_t{0});
        QCOMPARE(report.skippedCount(), std::size_t{0});
        QCOMPARE(report.files.front().exit_code, 0);
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
        QVERIFY(allocatedBytes < static_cast<std::uintmax_t>(contents.size()));
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
                const std::filesystem::path& fileToCompress) {
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
        QCOMPARE(progressUpdates.back().completed_files, std::size_t{1});
        QCOMPARE(progressUpdates.back().total_files, std::size_t{1});
        QCOMPARE(progressUpdates.back().processed_logical_bytes, std::uintmax_t{11});
        QVERIFY(progressUpdates.back().current_file.empty());
        QFile unchanged(filePath);
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCOMPARE(unchanged.readAll(), QByteArray("active data"));
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
};

QTEST_MAIN(MainWindowTests)
#include "ui_tests.moc"
