#pragma once

#include <QMainWindow>

class QLineEdit;

class MainWindow final : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void browseForDirectory();

    QLineEdit* directoryPath_ = nullptr;
};
