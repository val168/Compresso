#pragma once

#include "compresso/compression_backend.h"
#include "compresso/directory_scanner.h"

#include <QFutureWatcher>
#include <QMainWindow>
#include <QString>

class QLineEdit;
class QLabel;
class QPushButton;

class MainWindow final : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void browseForDirectory();
    void handleDirectoryTextChanged();
    void validateDirectoryPath();
    void startDirectoryScan();
    void displayScanResult();
    void startCompression();
    void displayCompressionResult();

    QLineEdit* directoryPath_ = nullptr;
    QLabel* originalSizeValue_ = nullptr;
    QLabel* compressedSizeValue_ = nullptr;
    QLabel* fileCountValue_ = nullptr;
    QLabel* filesystemValue_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QPushButton* analyzeButton_ = nullptr;
    QPushButton* compressButton_ = nullptr;
    QFutureWatcher<compresso::ScanResult>* scanWatcher_ = nullptr;
    QFutureWatcher<compresso::CompressionReport>* compressionWatcher_ = nullptr;
    QString validatedDirectory_;
    QString scanningDirectory_;
    QString compressingDirectory_;
    bool analyzedDirectoryIsEligible_ = false;
};
