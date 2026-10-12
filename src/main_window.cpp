#include "compresso/main_window.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("Compresso");
    resize(560, 300);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(10);

    auto* heading = new QLabel("Compresso", content);
    QFont headingFont = heading->font();
    headingFont.setPointSize(16);
    headingFont.setWeight(QFont::DemiBold);
    heading->setFont(headingFont);

    auto* description = new QLabel(
        "Apply transparent NTFS LZX compression to game folders while keeping files in place.",
        content);
    description->setWordWrap(true);

    auto* status = new QLabel("Compression is not implemented yet.", content);
    status->setWordWrap(true);

    layout->addWidget(heading);
    layout->addWidget(description);
    layout->addSpacing(8);
    layout->addWidget(status);
    layout->addStretch();

    setCentralWidget(content);
}
