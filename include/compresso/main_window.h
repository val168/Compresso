#pragma once

#include "compresso/compression_backend.h"
#include "compresso/directory_scanner.h"

#include <QMainWindow>
#include <QString>

class QLineEdit;
class QLabel;
class QPushButton;
class QThread;
class QCloseEvent;

class MainWindow final : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void startOperation(bool compressing, const QString& directory);
    void setOperationControlsEnabled(bool enabled);
    void browseForDirectory();
    void handleDirectoryTextChanged();
    void validateDirectoryPath();
    void startDirectoryScan();
    void displayScanResult(const compresso::ScanResult& result, const QString& directory);
    void startCompression();
    void displayCompressionResult(const compresso::CompressionReport& result,
                                  const QString& directory);

    QLineEdit* directoryPath_ = nullptr;
    QLabel* originalSizeValue_ = nullptr;
    QLabel* compressedSizeValue_ = nullptr;
    QLabel* fileCountValue_ = nullptr;
    QLabel* filesystemValue_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QPushButton* analyzeButton_ = nullptr;
    QPushButton* compressButton_ = nullptr;
    QThread* operationThread_ = nullptr;
    QString validatedDirectory_;
    bool closeAfterOperation_ = false;
    bool analyzedDirectoryIsEligible_ = false;
};
