#include "compresso/main_window.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName("Compresso");
    QApplication::setOrganizationName("Compresso");
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/resources/compresso-cup.png")));

    MainWindow window;
    window.show();
    window.refreshLibrary();

    return application.exec();
}
