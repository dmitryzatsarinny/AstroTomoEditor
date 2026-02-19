#include "..\Services\Pool.h"
#include "..\Window\Explorer\ExplorerDialog.h"
#include "..\Window\MainWindow\MainWindow.h"

#include <QSurfaceFormat>
#include <QVTKOpenGLNativeWidget.h>
#include <QStyleFactory>
#include <QSettings>
#include <Services/LanguageManager.h>
#include <Services/AppConfig.h>
#include <Services/TooltipsFilter.h>

static void ConfigureDllSearch()
{
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    AddDllDirectory(L".\\bin");
    AddDllDirectory(L".\\plugins");
    SetDllDirectoryW(L"");
    QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath() + "/plugins");
}

int main(int argc, char* argv[])
{
    ConfigureDllSearch();

    QCoreApplication::setOrganizationName("Astrocard");
    QCoreApplication::setApplicationName("AstroDicomEditor");
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    vtkOutputWindow::SetGlobalWarningDisplay(false);
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);

    app.setStyle(QStyleFactory::create("Fusion"));

    // === загрузка XML-конфига ===
    const QString cfgPath =
        QCoreApplication::applicationDirPath() + "/settings.xml";

    const AppConfig cfg = AppConfig::loadOrCreateDefault(cfgPath);

    // === установка языка ДО создания окон ===
    LanguageManager::instance().setLanguage(cfg.language);
    TooltipsFilter::instance().setEnabled(cfg.showTooltips);
    app.installEventFilter(&TooltipsFilter::instance());

    // --- если есть аргумент пути ---
    QString argPath;
    if (argc > 1) {
        argPath = QString::fromLocal8Bit(argv[1]);
        QFileInfo fi(argPath);

        if (fi.exists()) {
            ExplorerDialog::SelectionKind kind = ExplorerDialog::SelectionKind::None;

            // 3DR файл
            if (fi.isFile() && fi.suffix().compare("3dr", Qt::CaseInsensitive) == 0) {
                kind = ExplorerDialog::SelectionKind::File3DR;
            }
            // DICOMDIR
            else if (fi.isFile() && fi.fileName().compare("DICOMDIR", Qt::CaseInsensitive) == 0) {
                kind = ExplorerDialog::SelectionKind::DicomDir;
            }
            // Папка — проверим, DICOM ли она
            else if (fi.isDir()) {
                QDir d(argPath);
                const QStringList files = d.entryList(QDir::Files);

                bool hasDicom = false;
                for (const QString& f : files) {
                    if (f.endsWith(".dcm", Qt::CaseInsensitive) ||
                        f.endsWith(".dicom", Qt::CaseInsensitive))
                    {
                        hasDicom = true;
                        break;
                    }
                }

                if (hasDicom)
                    kind = ExplorerDialog::SelectionKind::DicomFolder;
            }

            // если тип определён — запускаем приложение без диалога
            if (kind != ExplorerDialog::SelectionKind::None) {
                MainWindow w(nullptr, argPath, kind);
                w.show();
                QTimer::singleShot(0, &w, &MainWindow::onOpenStudy);
                return app.exec();
            }
        }
    }


    ExplorerDialog dlg;
    if (dlg.exec() == QDialog::Accepted) {
        const auto kind = dlg.selectedKind();
        const QString path = dlg.selectedPath();

        switch (kind) {
        case ExplorerDialog::SelectionKind::DicomFile:
            qDebug() << "Selected DICOM-file:" << path;
            break;
        case ExplorerDialog::SelectionKind::DicomDir:
            qDebug() << "Selected DICOMDIR:" << path;
            break;
        case ExplorerDialog::SelectionKind::File3DR:
            qDebug() << "Selected 3DR:" << path;
            break;
        case ExplorerDialog::SelectionKind::DicomFolder:
            qDebug() << "Selected folder with DICOM-files:" << path;
            break;
        default:
            return 0;
            break;
        }

        if (!path.isEmpty())
        {
            MainWindow w(nullptr, path, kind);
            w.show();
            QTimer::singleShot(0, &w, &MainWindow::onOpenStudy);
            return app.exec();
        }
    }

    return 0;
}