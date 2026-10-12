#include "compresso/main_window.h"
#include "compresso/directory_validator.h"
#include "compresso/directory_scanner.h"

#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>
#include <fstream>
#include <algorithm>

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
        QVERIFY(originalSize);
        QVERIFY(compressedSize);
        QVERIFY(method);
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
                 QStringLiteral("Directory is ready. Compression is not implemented."));

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
        QCOMPARE(result.total_directory_size_bytes, std::uintmax_t{13});
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
        QCOMPARE(result.total_directory_size_bytes, std::uintmax_t{6});
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
        auto* status = window.findChild<QStatusBar*>("operationStatus");
        QVERIFY(pathField);
        QVERIFY(analyze);
        QVERIFY(originalSize);
        QVERIFY(fileCount);
        QVERIFY(compressedSize);
        QVERIFY(compress);
        QVERIFY(status);

        pathField->setText(temporaryDirectory.path());
        pathField->setFocus();
        QTest::keyClick(pathField, Qt::Key_Return);
        QVERIFY(analyze->isEnabled());
        QTest::mouseClick(analyze, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(status->currentMessage().startsWith("Scan complete:"), 5000);

        QCOMPARE(originalSize->text(), QStringLiteral("9 bytes"));
        QCOMPARE(fileCount->text(), QStringLiteral("1"));
        QCOMPARE(compressedSize->text(), QStringLiteral("—"));
        QVERIFY(!compress->isEnabled());
    }
};

QTEST_MAIN(MainWindowTests)
#include "ui_tests.moc"
