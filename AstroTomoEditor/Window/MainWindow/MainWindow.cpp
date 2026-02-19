#include "MainWindow.h"

#include "TitleBar.h"
#include "SeriesListPanel.h"
#include "PlanarView.h"
#include <QScopedValueRollback>
#include <Services/Save3DR.h>
#include <QApplication>
#include <QEvent>
#include <QElapsedTimer>
#include <Services/LanguageManager.h>
#include <Window/ServiceWindow/CustomMessageBox.h>

namespace 
{
    constexpr int kHeaderHeight = 34;
}

MainWindow::MainWindow(QWidget* parent,
    const QString& path,
    ExplorerDialog::SelectionKind kind)
    : QMainWindow(parent)
    , mDicomPath(path)
    , mKind(kind)
{
    setWindowTitle(tr("Astrocard DICOM Editor"));
    setMinimumSize(1069, 640);
    resize(1280, 800);
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_TranslucentBackground);

    setWindowIcon(QIcon(":/icons/Resources/dicom_heart.ico"));

#ifdef Q_OS_WIN
    if (auto* mb = menuBar()) mb->setNativeMenuBar(false);
#endif

    //qApp->installEventFilter(this);

    buildUi();
    mUiToDisable = mSplit;
    buildStyles();
    wireSignals();

    if (mTitle) { mTitle->set2DChecked(true); mTitle->set3DChecked(false); }
}

void MainWindow::buildUi()
{
    auto* frame = new QWidget(this);
    frame->setObjectName("WindowFrame");

    auto* outer = new QVBoxLayout(frame);
    outer->setContentsMargins(8, 8, 8, 8); // отступ от краёв окна до рамки
    outer->setSpacing(0);

    // внутренняя «карточка» со скруглёнными углами и рамкой
    auto* central = new QWidget(frame);
    central->setObjectName("CentralCard");

    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // title bar (наш кастомный)
    mTitle = new TitleBar(this);
    mTitle->setObjectName("TitleBar");          // важно для стилей
    v->addWidget(mTitle, 0);

    if (mTitle) { mTitle->set2DChecked(false); mTitle->set3DChecked(false); }

    // центральный сплиттер
    mSplit = new QSplitter(Qt::Horizontal, central);
    mSplit->setObjectName("MainSplit");
    mSplit->setHandleWidth(8);        // было 10
    mSplit->setChildrenCollapsible(false);

    // левая панель
    mSeries = new SeriesListPanel(mSplit);
    mSeries->setObjectName("SeriesPanel");
    mSeries->setMinimumWidth(100);
    mSeries->setMaximumWidth(300);
    mSeries->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    // правая область — стек просмотров
    mViewerStack = new QStackedWidget(mSplit);
    mViewerStack->setObjectName("ViewerStack");
    mViewerStack->setAutoFillBackground(false);
    mViewerStack->setAttribute(Qt::WA_TranslucentBackground);
    mViewerStack->setStyleSheet("background: transparent;");
    mViewerStack->setFrameStyle(QFrame::NoFrame);

    auto* placeholder = new QLabel(mViewerStack);
    placeholder->setAlignment(Qt::AlignCenter);
    placeholder->setStyleSheet("background: transparent; color: rgba(255,255,255,0.55);");

    mPlanar = new PlanarView(mViewerStack);
    mPlanar->setObjectName("PlanarView");
    mViewerStack->addWidget(mPlanar);

    // вид для 3D
    mRenderView = new RenderView(mViewerStack);
    mRenderView->setObjectName("RenderView");
    mViewerStack->addWidget(mRenderView);

    // по умолчанию показываем 2D
    mViewerStack->setCurrentWidget(mPlanar);

    mSplit->setStretchFactor(0, 0);
    mSplit->setStretchFactor(1, 1);

    v->addWidget(mSplit, 1);

    auto* mFooterSep = new QFrame(central);
    mFooterSep->setObjectName("FooterSep");
    mFooterSep->setFrameShape(QFrame::HLine);
    mFooterSep->setFrameShadow(QFrame::Plain);
    mFooterSep->setFixedHeight(1);
    v->addWidget(mFooterSep);

    // сам внутренний статус-бар (прозрачный)
    mFooter = new QWidget(central);
    mFooter->setObjectName("InnerStatusBar");
    mFooter->setFixedHeight(28);
    auto* fb = new QHBoxLayout(mFooter);
    fb->setContentsMargins(20, 4, 20, 4);
    fb->setSpacing(8);

    // прогресс (справа от текста)
    mProgBox = new QWidget(mFooter);
    auto* pbLay = new QHBoxLayout(mProgBox);
    pbLay->setContentsMargins(0, 0, 0, 0);
    constexpr int kProgWidth = 180;

    mProgress = new AsyncProgressBar(mProgBox);
    mProgress->setFixedHeight(4);
    mProgress->setFixedWidth(kProgWidth);
    mProgress->hideBar();              // внутреннее состояние Hidden
    mProgress->setVisible(false);      // по умолчанию не показываем

    // немного подправим палитру, чтобы полоска была светлой
    {
        QPalette pal = mProgress->palette();
        pal.setColor(QPalette::Window, QColor(0, 0, 0, 0));
        pal.setColor(QPalette::Base, QColor(0, 0, 0, 0));
        pal.setColor(QPalette::Highlight, QColor(230, 230, 230, 190));
        mProgress->setPalette(pal);
    }

    pbLay->addWidget(mProgress);
    
    // чтобы место сохранялось даже при hide()
    auto pol = mProgBox->sizePolicy();
    pol.setRetainSizeWhenHidden(true);
    mProgBox->setSizePolicy(pol);
    mProgBox->setFixedWidth(kProgWidth);
    mProgBox->setVisible(false);

    auto* leftMirror = new QWidget(mFooter);
    leftMirror->setFixedWidth(kProgWidth);
    leftMirror->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    // --- ЦЕНТР: текст в расширяющемся контейнере ---
    auto* centerWrap = new QWidget(mFooter);
    auto* cLay = new QHBoxLayout(centerWrap);
    cLay->setContentsMargins(0, 0, 0, 0);

    mStatusText = new QLabel(tr("Ready"), centerWrap);
    mStatusText->setAlignment(Qt::AlignCenter);
    mStatusText->setObjectName("StatusText");
    mStatusText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    cLay->addWidget(mStatusText);

    // --- Сборка футера: [лев.зеркало][центр][правый прогресс]
    fb->addWidget(leftMirror, 0);
    fb->addWidget(centerWrap, 1);
    fb->addWidget(mProgBox, 0, Qt::AlignRight);

    v->addWidget(mFooter, 0);

    mCornerGrip = new CornerGrip(mFooter);
    mCornerGrip->raise();

    outer->addWidget(central);
    mOuter = outer;
    mCentralCard = central;

    setCentralWidget(frame);

    auto* sb = new QStatusBar(this);
    sb->setSizeGripEnabled(false);
    sb->setFixedHeight(0);
    sb->setContentsMargins(0, 0, 0, 0);
    sb->setStyleSheet("QStatusBar{ background:transparent; border:0; margin:0; padding:0; }");
    setStatusBar(sb);
    sb->hide(); 

    if (!mPatientDlg)
        mPatientDlg = new PatientDialog(this);
    mPatientDlg->hide();

    if (!mSettingsDlg)
        mSettingsDlg = new SettingsDialog(this, true);
    mSettingsDlg->hide();

    connect(mSettingsDlg, &SettingsDialog::languageChanged, this, [](const QString& code)
        {
            LanguageManager::instance().setLanguage(code);
        });


    connect(mSettingsDlg, &SettingsDialog::volumeInterpolationChanged,
        this, [this](int mode)
        {
            if (!mRenderView) return;

            mRenderView->setVolumeInterpolation(
                mode == 1 ? RenderView::VolumeInterpolation::Linear
                : RenderView::VolumeInterpolation::Nearest);
        });

    retranslateUi(true);
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
        this, [this] { retranslateUi(false); });
}

void MainWindow::retranslateUi(bool loading)
{
    setWindowTitle(tr("Astrocard DICOM Editor"));

    if (mStatusText)
        mStatusText->setText(tr("Ready"));

    if (mTitle)
        mTitle->retranslateUi();   // добавим ниже


    if (mTitle)
    {
        if (!loading)
        mTitle->setPatientInfo(mCurrentPatient);
    }

    if (mPatientDlg)
        mPatientDlg->retranslateUi();

    if (mPlanar)
        mPlanar->retranslateUi();

    if (mSeries)
        mSeries->retranslateUi();

    if(mSettingsDlg)
        mSettingsDlg->retranslateUi();
}


void MainWindow::showEvent(QShowEvent* e) {
    QMainWindow::showEvent(e);
    positionCornerGrip();      // на случай показа после построения
}

void MainWindow::buildStyles()
{
    QString ss;

    // Карточка рисует ВЕСЬ внешний контур
    ss += "#CentralCard {"
        "  background:#1f2023;"
        "  border:1px solid rgba(255,255,255,0.14);"
        "  border-radius:10px;"
        "}\n";


    // Прозрачный внутренний статус-бар — никаких рамок и углов
    ss += "#InnerStatusBar {"
        "  background: transparent;"
        "  border: none;"
        "}\n";

    // Тонкая разделительная линия над футером
    ss += "#FooterSep {"
        "  background: rgba(255,255,255,0.10);"
        "  border: none;"
        "  margin: 0;"
        "}\n";

    //ss += "#ViewerStack, #ViewerStack * { background: transparent; } \n";
    ss += "#ViewerStack {"
        "  background: rgba(255,255,255,0.10);"
        "  margin: 10px;"
        "}\n";

    // Заголовок как был
    ss += "#TitleBar {"
        "  background:#1e1f22;"
        "  border-bottom:1px solid rgba(255,255,255,0.12);"
        "  border-top-left-radius:10px;"
        "  border-top-right-radius:10px;"
        "}\n";

    ss += R"(
        /* Общие настройки для элементов */
        #SeriesPanel QListWidget {
            background: rgba(255,255,255,0.02);
            border-radius: 8px;
            outline: none;
            border: none;
        }
        
        #SeriesPanel QListWidget::item {
            background: rgba(255,255,255,0.03);
            border-radius: 8px;
            margin: 2px 2px;
            padding: 6px;
            color: #ddd;
        }
        
        /* Когда курсор над элементом */
        #SeriesPanel QListWidget::item:hover {
            background: rgba(255,255,255,0.10);
            border: 1px solid rgba(255,255,255,0.14);
        }
        
        /* Активный / выделенный элемент */
        #SeriesPanel QListWidget::item:selected {
            background: rgba(255,255,255,0.15);
            border: 2px solid rgba(255,255,255,0.25);
        }
        
        /* Миниатюры в элементах */
        #SeriesPanel QListWidget::icon {
            margin: 4px;
        }
        
        /* Текст */
        #SeriesPanel QListWidget::item:selected:!active {
            color: white;
        }
        
        /* Вертикальный скролл компактный */
        #SeriesPanel QScrollBar:vertical {
            background: transparent;
            width: 8px;
            margin: 4px 0 4px 0;
        }
        
        #SeriesPanel QScrollBar::handle:vertical {
            background: rgba(255,255,255,0.18);
            min-height: 24px;
            border-radius: 4px;
        }
        
        #SeriesPanel QScrollBar::handle:vertical:hover {
            background: rgba(255,255,255,0.32);
        }
        
        #SeriesPanel QScrollBar::add-line:vertical,
        #SeriesPanel QScrollBar::sub-line:vertical {
            height: 0;
        }
        )";


    ss += R"(
            
                /* Аккуратный хэндл сплиттера — тонкая полоска по центру */
                #MainSplit::handle:horizontal {
                    background: qlineargradient(
                        x1:0, y1:0, x2:1, y2:0,
                        stop:0   rgba(0,0,0,0),
                        stop:0.46 rgba(255,255,255,0.09),
                        stop:0.54 rgba(255,255,255,0.09),
                        stop:1   rgba(0,0,0,0)
                    );
                    width: 8px;
                }
            
                #MainSplit::handle:horizontal:hover {
                    background: qlineargradient(
                        x1:0, y1:0, x2:1, y2:0,
                        stop:0   rgba(0,0,0,0),
                        stop:0.46 rgba(255,255,255,0.18),
                        stop:0.54 rgba(255,255,255,0.18),
                        stop:1   rgba(0,0,0,0)
                    );
                }
            
                #MainSplit::handle:horizontal:pressed {
                    background: qlineargradient(
                        x1:0, y1:0, x2:1, y2:0,
                        stop:0   rgba(0,0,0,0),
                        stop:0.46 rgba(255,255,255,0.26),
                        stop:0.54 rgba(255,255,255,0.26),
                        stop:1   rgba(0,0,0,0)
                    );
                }
            )";

    ss += "QLabel#StatusText { color: rgba(255,255,255,0.95); }\n";

    // Если окно развёрнуто — без радиусов
    ss += "#CentralCard[maxed=\"true\"] { border-radius:0; }"
        "#TitleBar[maxed=\"true\"] { border-top-left-radius:0; border-top-right-radius:0; }\n";

    qApp->setStyleSheet(ss);
}

void MainWindow::wireSignals()
{
    // TitleBar
    connect(mTitle, &TitleBar::patientClicked,
        this, &MainWindow::showPatientDetails);

    connect(mTitle, &TitleBar::settingsClicked,
        this, &MainWindow::showSettings);

    // Series panel → header / viewer / scan progress
    connect(mSeries, &SeriesListPanel::patientInfoChanged,
        this, &MainWindow::onSeriesPatientInfoChanged);

    // при активации серии: переключить вид и обновить статус
    connect(mSeries, &SeriesListPanel::seriesActivated,
        this, &MainWindow::onSeriesActivated);

    // напрямую прокинем файлы в PlanarView (если у него есть соответствующий слот — оставим через лямбду)
    connect(mSeries, &SeriesListPanel::seriesActivated,
        this, [this](const QString&, const QVector<QString>& files) 
        {
            if (mPlanar && !files.isEmpty())
                if (!mPlanar->IsLoading())
                {
                    StartLoading();
                    mPlanar->StartLoading();
                    mPlanar->loadSeriesFiles(files);
                }
        });

    // прогресс сканирования (левая панель)
    connect(mRenderView, &RenderView::renderStarted, this,
        [this]() {
            mStatusText->setText(tr("Render 0%"));
            StartLoading();
            mProgBox->setVisible(true);
            if (mProgress) {
                mProgress->setVisible(true);
                mProgress->startFill();                     // детерминированный режим
                mProgress->setRange(0, 100);
                mProgress->setValue(0);
            }
        });

    connect(mRenderView, &RenderView::renderProgress, this,
        [this](int processed) {
            mStatusText->setText(tr("Render progress: %1").arg(processed));
            if (mProgress) {
                mProgress->setRange(0, std::max(1, 100));
                mProgress->setValue(std::clamp(processed, 0, 100));
            }
        });

    connect(mRenderView, &RenderView::Progress, this,
        [this](int processed) {
            if (processed == 0)
            {
                StartLoading();
                mProgBox->setVisible(true);
                if (mProgress) 
                {
                    mProgress->setVisible(true);
                    mProgress->startLoading();
                    mProgress->setRange(0, 100);
                    mProgress->setValue(0);
                }
            }
            else if (processed < 100)
            {
                if (mProgress)
                {
                    mProgress->setRange(0, 100);
                    mProgress->setValue(processed);
                }
            }
            else
            {
                if (mProgress) 
                {
                    mProgress->setValue(mProgress->maximum());
                    mProgress->hideBar();
                    mProgress->setVisible(false);
                }
                StopLoading();
            }
            qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        });

    connect(mRenderView, &RenderView::renderFinished, this,
        [this]() {
            mStatusText->setText(tr("Render success"));
            if (mProgress) {
                mProgress->setValue(mProgress->maximum());
                mProgress->hideBar();
                mProgress->setVisible(false);
            }
            StopLoading();
            QTimer::singleShot(800, this, [this] {
                mProgBox->setVisible(false);
                mStatusText->setText(tr("Ready"));
                });
        });

    connect(mRenderView, &RenderView::showInfo,
        this, &MainWindow::showInfo);

    connect(mSeries, &SeriesListPanel::scanStarted, this,
        [this](int total) {
            mStatusText->setText(tr("DICOM files detection 0%"));
            StartLoading();
            mProgBox->setVisible(true);
            if (mProgress) {
                mProgress->setVisible(true);
                if (total > 0) {
                    mProgress->startFill();
                    mProgress->setRange(0, std::max(1, total));
                    mProgress->setValue(0);
                }
                else {
                    mProgress->startLoading();    // пока не знаем total
                }
            }
        });

    connect(mSeries, &SeriesListPanel::scanProgress, this,
        [this](int processed, int total, const QString& path) {

            if (!mProgress)
                return;

            if (total <= 0) {
                mStatusText->setText(tr("Searching… %1 files checked: %2")
                    .arg(processed)
                    .arg(QFileInfo(path).fileName()));
                mProgress->startLoading();         // чистый indeterminate
                return;
            }

            const int pct = (total > 0) ? (processed * 100 / total) : 0;
            mStatusText->setText(tr("Header reading: %1 (%2%)")
                .arg(QFileInfo(path).fileName())
                .arg(pct));

            mProgress->startFill();
            mProgress->setRange(0, std::max(1, total));
            mProgress->setValue(std::min(processed, total));
        });

    connect(mSeries, &SeriesListPanel::scanFinished, this,
        [this](int seriesCount, int /*total*/) {
            mStatusText->setText(tr("Ready. Series: %1").arg(seriesCount));
            if (mProgress) {
                mProgress->setValue(mProgress->maximum());
                mProgress->hideBar();
                mProgress->setVisible(false);
            }
            StopLoading();
            QTimer::singleShot(800, this, [this] {
                mProgBox->setVisible(false);
                mStatusText->setText(tr("Ready"));
                });
        });

    // прогресс загрузки в PlanarView (правый просмотр)
    connect(mPlanar, &PlanarView::loadStarted, this,
        [this](int total) {
            mViewerStack->setCurrentWidget(mPlanar);
            mStatusText->setText(tr("Series loading… 0/%1").arg(total));
            mProgBox->setVisible(true);
            if (mProgress) {
                mProgress->setVisible(true);
                mProgress->startFill();
                mProgress->setRange(0, std::max(1, total));
                mProgress->setValue(0);
            }
        });

    connect(mPlanar, &PlanarView::loadProgress, this,
        [this](int processed, int total) {
            mStatusText->setText(tr("Series loading… %1/%2").arg(processed).arg(total));
            if (mProgress) {
                mProgress->setRange(0, std::max(1, total));
                mProgress->setValue(std::min(processed, total));
            }
        });

    connect(mPlanar, &PlanarView::loadFinished, this,
        [this](int total) {
            mStatusText->setText(tr("Loaded: %1 slices").arg(total));
            if (mProgress) {
                mProgress->setValue(mProgress->maximum());
                mProgress->hideBar();
                mProgress->setVisible(false);
            }
            mPlanar->StopLoading();
            StopLoading();
            onShowPlanar2D();
            QTimer::singleShot(1200, this, [this] {
                mProgBox->setVisible(false);
                mStatusText->setText(tr("Ready"));
                });
        });

    connect(mSettingsDlg, &SettingsDialog::gradientOpacityChanged,
        mRenderView, &RenderView::setGradientOpacityEnabled);

    connect(mRenderView, &RenderView::gradientOpacityChanged,
        mSettingsDlg, &SettingsDialog::syncGradientOpacityUi);

    connect(mTitle, &TitleBar::volumeClicked, this, &MainWindow::onShowVolume3D);
    connect(mTitle, &TitleBar::planarClicked, this, &MainWindow::onShowPlanar2D);

    // Подписка на клик из заголовка
    connect(mTitle, &TitleBar::save3DRRequested, this, &MainWindow::onSave3DR);

    // Горячая клавиша Ctrl+S (по желанию)
    auto* actSave = new QAction(tr("Save 3DR"), this);
    actSave->setShortcut(QKeySequence::Save);
    connect(actSave, &QAction::triggered, this, &MainWindow::onSave3DR);
    addAction(actSave);

    auto sc2 = new QShortcut(QKeySequence(Qt::Key_2), this);
    connect(sc2, &QShortcut::activated, this, [this]
        {
            if (!mPlanar)
                return;

            if (mTitle)
                if (!mTitle->is2DVisible())
                    return;

            onShowPlanar2D();
        });

    auto sc3 = new QShortcut(QKeySequence(Qt::Key_3), this);
    connect(sc3, &QShortcut::activated, this, [this]    // ← sc3 здесь
        {
            if (!mPlanar)
                return;

            if (mTitle)
                if (!mTitle->is3DVisible())
                    return;

            onShowVolume3D();
        });
}

void MainWindow::onSave3DR()
{
    if (!mRenderView) return;

    auto vol = mRenderView->image();
    if (!vol) 
    {
        CustomMessageBox::warning(this, tr("Save 3DR"), tr("No volume to save"), ServiceWindow);
        return;
    }

    QString filename = "NULL";
    DicomInfo di = mRenderView->GetDicomInfo();
    if (Save3DR::saveWithDialog(this, mRenderView->image(), &di, filename))
        mRenderView->saveTemplates(filename);
}

void MainWindow::StartLoading()
{
    if (mLoading)
        return;

    mLoading = true;
    mUiToDisable = centralWidget();

    QObject* src = sender();
    std::optional<QSignalBlocker> blocker;
    if (src) blocker.emplace(src);

    if (mUiToDisable) mUiToDisable->setEnabled(false);
    QApplication::setOverrideCursor(Qt::BusyCursor);
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
}

void MainWindow::StopLoading()
{
    // Снять override-курсор(ы) со стека
    while (QApplication::overrideCursor())
        QApplication::restoreOverrideCursor();

    // Пока идёт загрузка, чуть подчистим очередь событий,
    // чтобы накопленные клики/скроллы не выстрелили сразу после разблокировки.
    QElapsedTimer flushTimer;
    flushTimer.start();

    // крутимся не дольше 50 мс, просто обрабатывая все события
    while (mLoading && flushTimer.elapsed() < 50)
    {
        qApp->processEvents(QEventLoop::AllEvents);
    }

    if (mUiToDisable)
        mUiToDisable->setEnabled(true);

    mLoading = false;

    // Ещё раз обновим UI, но без пользовательского ввода
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
}


void MainWindow::onOpenStudy()
{
    if (mTitle) { mTitle->set2DChecked(true); mTitle->set3DChecked(false); }

    mPlanar->sethidescroll();
    mProgBox->setVisible(true);
    if (mProgress) {
        mProgress->setVisible(true);
        mProgress->startLoading();  // чистый indeterminate
    }
    mStatusText->setText(tr("Searching DICOM files…"));
    StartLoading();

    qApp->processEvents(QEventLoop::AllEvents);
    QTimer::singleShot(0, this, &MainWindow::startScan);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (mLoading)
    {
        switch (event->type())
        {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::DragMove:
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::ContextMenu:
            return true; // игнорируем пользовательский ввод пока идёт загрузка
        default:
            break;
        }
    }

    return QMainWindow::eventFilter(obj, event);
}


void MainWindow::showInfo(const QString& text)
{
    if (mStatusText) mStatusText->setText(text);
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
}

void MainWindow::startScan()
{
    switch (mKind)
    {
    case ExplorerDialog::SelectionKind::DicomDir:
        mSeries->scanDicomDir(mDicomPath);
        break;
    case ExplorerDialog::SelectionKind::DicomFile:
        mSeries->scanSingleFile(mDicomPath);
        break;
    case ExplorerDialog::SelectionKind::File3DR:
        mSeries->scan3drFile(mDicomPath);
        break;
    case ExplorerDialog::SelectionKind::DicomFolder:
    default:
        mSeries->scanStudy(mDicomPath);
        break;
    }
    emit studyOpened(mDicomPath);
}

void MainWindow::changeEvent(QEvent* e)
{
    QMainWindow::changeEvent(e);

    if (e->type() == QEvent::LanguageChange)
    {
        retranslateUi(false);
        return;
    }

    if (e->type() == QEvent::WindowStateChange)
    {
        applyMaximizedUi(isMaximized());
        positionCornerGrip();
    }
}

void MainWindow::resizeEvent(QResizeEvent* e)
{
    QMainWindow::resizeEvent(e);
    positionCornerGrip();
}

void MainWindow::positionCornerGrip()
{
    if (!mFooter || !mCornerGrip)
        return;

    const int insetX = 2;
    const int insetY = 2;
    const int x = mFooter->width() - mCornerGrip->width() - insetX;
    const int y = mFooter->height() - mCornerGrip->height() - insetY;
    mCornerGrip->move(x, y);
}

void MainWindow::applyMaximizedUi(bool maxed)
{
    if (mOuter) mOuter->setContentsMargins(maxed ? 0 : 8, maxed ? 0 : 8,
        maxed ? 0 : 8, maxed ? 0 : 8);

    // Проставим property, чтобы стиль подхватил разные радиусы
    if (mCentralCard) { mCentralCard->setProperty("maxed", maxed); mCentralCard->style()->unpolish(mCentralCard); mCentralCard->style()->polish(mCentralCard); }
    if (mTitle) { mTitle->setProperty("maxed", maxed);       mTitle->style()->unpolish(mTitle);             mTitle->style()->polish(mTitle); }
    if (mCornerGrip) mCornerGrip->setVisible(!maxed);
}

void MainWindow::onExitRequested()
{
    close();
}

// ===== Reactions from panels ===============================================

void MainWindow::onSeriesPatientInfoChanged(const PatientInfo& info)
{
    mCurrentPatient = info;
    if (mTitle) mTitle->setPatientInfo(mCurrentPatient);
    if (mPatientDlg) mPatientDlg->setInfo(mCurrentPatient);
}

void MainWindow::onSeriesActivated(const QString& /*seriesUID*/, const QVector<QString>& files)
{
    StartLoading();

    if (!mPlanar || files.isEmpty())
        return;

    if (mPlanar->IsLoading())
        return;

    mPlanar->sethidescroll();

    if (mRenderView)
        mRenderView->hideOverlays();

    if (mTitle) { mTitle->set2DVisible(false); mTitle->set3DVisible(false); mTitle->set3DChecked(false); mTitle->set2DChecked(false); mTitle->setSaveVisible(false);}

    mViewerStack->setCurrentWidget(mPlanar);
    mStatusText->setText(tr("Series loading…"));
}

void MainWindow::onShowVolume3D()
{
    if (!mPlanar) return;

    auto vtkVol = mPlanar->makeVtkVolume();
    if (!vtkVol) {
        mStatusText->setText(tr("Warning"));
        return;
    }

    mRenderView->setVolume(vtkVol, mPlanar->GetDicomInfo(), mCurrentPatient);
    mViewerStack->setCurrentWidget(mRenderView);

    if (mTitle) { mTitle->set3DChecked(true); mTitle->set2DChecked(false); }
    mStatusText->setText(tr("Ready volume"));

    mTitle->setSaveVisible(true);
}

void MainWindow::onShowPlanar2D()
{
    if (mPlanar) 
    {
        mViewerStack->setCurrentWidget(mPlanar);

        if (mRenderView)
            mRenderView->hideOverlays();

        // <<< здесь обновляем состояние кнопок TitleBar
        if (mTitle) { mTitle->set2DChecked(true); mTitle->set3DChecked(false); }

        mTitle->setSaveVisible(false);

        mTitle->set2DVisible(true);
        if (mPlanar->IsAvalibleToReconstruct())
        mTitle->set3DVisible(true);
    }
}

void MainWindow::showPatientDetails()
{
    if (mCurrentPatient.patientName.isEmpty())
        return;

    if (!mPatientDlg)
        mPatientDlg = new PatientDialog(this);

    mPatientDlg->setInfo(mCurrentPatient);

    mPatientDlg->show();
    mPatientDlg->raise();
    mPatientDlg->activateWindow();

    // Центрируем относительно главного окна
    const QRect r = geometry();
    const QSize s = mPatientDlg->size();
    mPatientDlg->move(r.center() - QPoint(s.width() / 2, s.height() / 2 + 40));
}

void MainWindow::showSettings()
{
    if (!mSettingsDlg)
        mSettingsDlg = new SettingsDialog(this, true);


    mSettingsDlg->show();
    mSettingsDlg->raise();
    mSettingsDlg->activateWindow();

    // Центрируем относительно главного окна
    const QRect r = geometry();
    const QSize s = mSettingsDlg->size();
    mSettingsDlg->move(r.center() - QPoint(s.width() / 2, s.height() / 2 + 40));
}