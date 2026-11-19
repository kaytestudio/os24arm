#include <QApplication>
#include <QStyleFactory>
#include "installer/InstallerWizard.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("OS/2 Warp 4.52 Installer");
    app.setApplicationVersion("4.52");
    app.setOrganizationName("IBM");

    QFile styleFile(":/styles/os2theme.qss");
    if (styleFile.open(QFile::ReadOnly)) {
        app.setStyleSheet(styleFile.readAll());
        styleFile.close();
    }

    InstallerWizard wizard;
    wizard.show();

    return app.exec();
}
