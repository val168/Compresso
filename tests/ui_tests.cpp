#include "compresso/main_window.h"
#include "compresso/directory_validator.h"

#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QtTest>

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

    void leavesUnimplementedOperationsDisabled()
    {
        MainWindow window;

        const auto* browse = window.findChild<QPushButton*>("browseButton");
        QVERIFY(browse);
        QVERIFY(browse->isEnabled());

        for (const auto* name : {"analyzeButton", "compressButton", "restoreButton"}) {
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
};

QTEST_MAIN(MainWindowTests)
#include "ui_tests.moc"
