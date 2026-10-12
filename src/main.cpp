#include "compresso/main_window.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName("Compresso");
    QApplication::setOrganizationName("Compresso");

    MainWindow window;
    window.show();

    return application.exec();
}
