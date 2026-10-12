#pragma once

#include <QMainWindow>
#include <QString>

class QLineEdit;
class QLabel;

class MainWindow final : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void browseForDirectory();
    void handleDirectoryTextChanged();
    void validateDirectoryPath();

    QLineEdit* directoryPath_ = nullptr;
    QLabel* originalSizeValue_ = nullptr;
    QLabel* compressedSizeValue_ = nullptr;
    QString validatedDirectory_;
};
