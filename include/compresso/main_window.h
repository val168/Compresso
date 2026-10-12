#pragma once

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

    QLineEdit* directoryPath_ = nullptr;
    QLabel* originalSizeValue_ = nullptr;
    QLabel* compressedSizeValue_ = nullptr;
    QLabel* fileCountValue_ = nullptr;
    QLabel* filesystemValue_ = nullptr;
    QPushButton* analyzeButton_ = nullptr;
    QFutureWatcher<compresso::ScanResult>* scanWatcher_ = nullptr;
    QString validatedDirectory_;
    QString scanningDirectory_;
};
