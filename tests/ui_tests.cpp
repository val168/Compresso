#include "compresso/main_window.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStatusBar>
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
};

QTEST_MAIN(MainWindowTests)
#include "ui_tests.moc"
