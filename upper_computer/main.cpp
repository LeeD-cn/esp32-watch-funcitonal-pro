#include <QApplication>

#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("ESP-WATCH 上位机");
    QApplication::setOrganizationName("ESP-WATCH");

    MainWindow window;
    window.show();
    return app.exec();
}
