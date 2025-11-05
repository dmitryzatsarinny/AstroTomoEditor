#include "MainWindow.h"

#include "TitleBar.h"
#include "SeriesListPanel.h"
#include "PlanarView.h"
#include <QScopedValueRollback>
#include <Services/Save3DR.h>

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
    setMinimumSize(960, 640);
    resize(1280, 800);
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_TranslucentBackground);

    setWindowIcon(QIcon(":/icons/Resources/dicom_heart.png"));

#ifdef Q_OS_WIN
    if (auto* mb = menuBar()) mb->setNativeMenuBar(false);
#endif

    buildUi();
    mUiToDisable = centralWidget();
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
    mSplit->setHandleWidth(2);
    mSplit->setChildrenCollapsible(false);

    // левая панель
    mSeries = new SeriesListPanel(mSplit);
    mSeries->setObjectName("SeriesPanel");
    mSeries->setMinimumWidth(260);
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

    mProgress = new QProgressBar(mProgBox);
    mProgress->setTextVisible(false);
    mProgress->setFixedHeight(4);
    mProgress->setFixedWidth(kProgWidth);
    mProgress->setRange(0, 100);
    mProgress->setValue(0);
    mProgBox->setVisible(false);
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
    mStatusText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    cLay->addWidget(mStatusText);

    // --- Сборка футера: [лев.зеркало][центр][правый прогресс]
    fb->addWidget(leftMirror, 0);
    fb->addWidget(centerWrap, 1);
    fb->addWidget(mProgBox, 0, Qt::AlignRight);

    v->addWidget(mFooter, 0);

    mCornerGrip = new CornerGrip(mFooter);
    mCornerGrip->raise();
    //QTimer::singleShot(0, this, [this] { positionCornerGrip(); });

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

    // Если окно развёрнуто — без радиусов
    ss += "#CentralCard[maxed=\"true\"] { border-radius:0; }"
        "#TitleBar[maxed=\"true\"] { border-top-left-radius:0; border-top-right-radius:0; }\n";

    // Вся карточка: как и раньше, общий контур и скругление
    ss += "#PatientDialogCard{"
        " background:#1f2023;"
        " border:1px solid rgba(255,255,255,0.14);"
        " border-radius:10px;"
        "}\n";

    // Шапка с собственным контуром (как у #CentralCard) и нижней разделительной линией.
    // Небольшие отрицательные отступы «вклеивают» контур шапки в общий контур карточки,
    // так что визуально это одна линия по верху и бокам.
    ss += "#PD_TitleBar{"
        " background:#1e1f22;"
        " border: none;"         /* общий контур шапки */
        " border-bottom:1px solid rgba(255,255,255,0.12);"  /* тонкая линия отделяет шапку от тела */
        " border-top-left-radius:9px;"
        " border-top-right-radius:9px;"
        " margin:1px 1px 0 1px;"                         /* приклеиваем к контуру карточки */
        " padding:6px 6px 6 10px;"
        "}\n";

    ss += "#PD_Title{ color:#e6e6e6; font-size:14px; font-weight:600; }\n";

    // Кнопка закрытия — та же логика, что в главном окне: прозрачная и «сереет» при hover
    ss += "#PD_Close{"
        " border:none; padding:2px; border-radius:6px;"
        " background:transparent; color:#e6e6e6;"
        "}"
        "#PD_Close:hover{ background:rgba(255,255,255,0.10); }"
        "#PD_Close:pressed{ background:rgba(255,255,255,0.16); }\n";

    // Тело без внутренних рамок
    ss += "#PD_Body{ background:transparent; border:none; }\n";

    qApp->setStyleSheet(ss);
}

void MainWindow::wireSignals()
{
    // TitleBar
    connect(mTitle, &TitleBar::patientClicked,
        this, &MainWindow::showPatientDetails);

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
            mProgress->setValue(0);
        });

    connect(mRenderView, &RenderView::renderProgress, this,
        [this](int processed) {
            mStatusText->setText(tr("Render progress: %1")
                .arg(processed));
            mProgress->setRange(0, std::max(1, 100));
            mProgress->setValue(std::min(processed, processed));
        });

    connect(mRenderView, &RenderView::renderFinished, this,
        [this]() {
            mStatusText->setText(tr("Render success"));
            mProgress->setValue(mProgress->maximum());
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
            mProgress->setRange(0, std::max(1, total));
            mProgress->setValue(0);
        });

    connect(mSeries, &SeriesListPanel::scanProgress, this,
        [this](int processed, int total, const QString& path) {
            const int pct = (total > 0) ? (processed * 100 / total) : 0;
            mStatusText->setText(tr("Header reading: %1 (%2%)")
                .arg(QFileInfo(path).fileName())
                .arg(pct));
            mProgress->setRange(0, std::max(1, total));
            mProgress->setValue(std::min(processed, total));
        });

    connect(mSeries, &SeriesListPanel::scanFinished, this,
        [this](int seriesCount, int /*total*/) {
            mStatusText->setText(tr("Ready. Series: %1").arg(seriesCount));
            mProgress->setValue(mProgress->maximum());
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
            mProgress->setRange(0, std::max(1, total));
            mProgress->setValue(0);
        });

    connect(mPlanar, &PlanarView::loadProgress, this,
        [this](int processed, int total) {
            mStatusText->setText(tr("Series loading… %1/%2").arg(processed).arg(total));
            mProgress->setRange(0, std::max(1, total));
            mProgress->setValue(std::min(processed, total));
        });

    connect(mPlanar, &PlanarView::loadFinished, this,
        [this](int total) {
            mStatusText->setText(tr("Loaded: %1 slices").arg(total));
            mProgress->setValue(mProgress->maximum());
            mPlanar->StopLoading();
            StopLoading();
            onShowPlanar2D();
            QTimer::singleShot(1200, this, [this] 
                {
                mProgBox->setVisible(false);
                mStatusText->setText(tr("Ready"));
                });
        });


    connect(mTitle, &TitleBar::volumeClicked, this, &MainWindow::onShowVolume3D);
    connect(mTitle, &TitleBar::planarClicked, this, &MainWindow::onShowPlanar2D);

    // Подписка на клик из заголовка
    connect(mTitle, &TitleBar::save3DRRequested, this, &MainWindow::onSave3DR);

    // Горячая клавиша Ctrl+S (по желанию)
    auto* actSave = new QAction(tr("Сохранить 3DR"), this);
    actSave->setShortcut(QKeySequence::Save);
    connect(actSave, &QAction::triggered, this, &MainWindow::onSave3DR);
    addAction(actSave);
}

void MainWindow::onSave3DR()
{
    if (!mRenderView) return;

    auto vol = mRenderView->image();
    if (!vol) {
        QMessageBox::warning(this, tr("Сохранение 3DR"), tr("Нет объёма для сохранения."));
        return;
    }

    DicomInfo di = mRenderView->GetDicomInfo();
    Save3DR::saveWithDialog(this, mRenderView->image(), &di);
}

void MainWindow::StartLoading()
{
    if (mLoading)
        return;
    QScopedValueRollback<bool> lk(mLoading, true);

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

    if (mUiToDisable) mUiToDisable->setEnabled(true);
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
}

void MainWindow::onOpenStudy()
{
    if (mTitle) { mTitle->set2DChecked(true); mTitle->set3DChecked(false); }
    mStatusText->setText(tr("Prepare to scan. Please wait."));
    qApp->processEvents(QEventLoop::AllEvents);

    // Запуск на следующем тике
    QTimer::singleShot(0, this, &MainWindow::startScan);
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
    if (mTitle) mTitle->setPatientInfo(info);
}

void MainWindow::onSeriesActivated(const QString& /*seriesUID*/, const QVector<QString>& files)
{
    StartLoading();

    if (!mPlanar || files.isEmpty())
        return;

    if (mPlanar->IsLoading())
        return;

    if (mRenderView)
        mRenderView->hideOverlays();

    if (mTitle) { mTitle->set2DVisible(false); mTitle->set3DVisible(false); mTitle->set3DChecked(false); mTitle->set2DChecked(false); mTitle->setSaveVisible(false);}

    mViewerStack->setCurrentWidget(mPlanar);
    mStatusText->setText(tr("Series loading…"));
}

void MainWindow::ensurePatientDialog()
{
    if (mPatientDlg) return;

    // Фрейм без рамки и с альфа-каналом
    QWidget* dlg = new QWidget(this, Qt::FramelessWindowHint | Qt::Tool);
    dlg->setObjectName("PatientDialogFrame");
    dlg->setAttribute(Qt::WA_TranslucentBackground);
    dlg->setFixedSize(420, 210);
    mPatientDlg = dlg;

    // Внешний отступ от края окна до карточки
    auto* outer = new QVBoxLayout(dlg);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(0);

    // Внутренняя «карточка»
    QWidget* card = new QWidget(dlg);
    card->setObjectName("PatientDialogCard");
    outer->addWidget(card);

    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // Заголовок
    auto* title = new QWidget(card);
    title->setObjectName("PD_TitleBar");
    title->setMouseTracking(true);
    title->installEventFilter(dlg);
    dlg->installEventFilter(dlg); 

    card->setAttribute(Qt::WA_StyledBackground, true);
    title->setAttribute(Qt::WA_StyledBackground, true);

    class PDDragFilter : public QObject {
        QWidget* w_;
    public:
        explicit PDDragFilter(QWidget* w) : QObject(w), w_(w) {}
    protected:
        bool eventFilter(QObject* obj, QEvent* e) override {
            static bool dragging = false;
            static QPoint startPos;
            if (e->type() == QEvent::MouseButtonPress) {
                auto* me = static_cast<QMouseEvent*>(e);
                if (me->button() == Qt::LeftButton) {
                    dragging = true;
                    startPos = me->globalPosition().toPoint() - w_->frameGeometry().topLeft();
                    return true;
                }
            }
            else if (e->type() == QEvent::MouseMove) {
                if (dragging) {
                    auto* me = static_cast<QMouseEvent*>(e);
                    w_->move(me->globalPosition().toPoint() - startPos);
                    return true;
                }
            }
            else if (e->type() == QEvent::MouseButtonRelease) {
                dragging = false;
            }
            return QObject::eventFilter(obj, e);
        }
    };

    title->installEventFilter(new PDDragFilter(dlg));

    // локальные замыкания-хранилища
    static QPoint s_dragPos;
    static bool   s_dragging = false;

    auto* th = new QHBoxLayout(title);
    th->setContentsMargins(10, 6, 6, 6);
    th->setSpacing(6);
    auto* tlabel = new QLabel(tr("Patient Data"), title);
    tlabel->setObjectName("PD_Title");
    th->addWidget(tlabel);
    th->addStretch();

    dlg->connect(title, &QWidget::customContextMenuRequested, [] {}); // чтобы у компоновщика был объект

    dlg->QObject::connect(dlg, &QObject::destroyed, dlg, [] { s_dragging = false; });

    dlg->QObject::connect(title, &QWidget::windowTitleChanged, dlg, [] {}); // заглушка — чтобы у moc были слоты

    auto* closeBtn = new QToolButton(title);
    closeBtn->setObjectName("PD_Close");
    closeBtn->setText("✕");                 // или тот же QIcon, что в TitleBar
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setFixedSize(22, 22);         // аккуратный размер
    closeBtn->setToolTip(tr("Close"));

    th->addWidget(closeBtn);

    connect(closeBtn, &QToolButton::clicked, dlg, &QWidget::close);

    v->addWidget(title, 0);

    // Контент
    QWidget* body = new QWidget(card);
    body->setObjectName("PD_Body");
    auto* form = new QFormLayout(body);
    form->setContentsMargins(16, 10, 16, 14);
    form->setSpacing(8);

    mPD_Name = new QLabel(body);
    mPD_Id = new QLabel(body);
    mPD_Sex = new QLabel(body);
    mPD_Birth = new QLabel(body);

    form->addRow(tr("Name:"), mPD_Name);
    form->addRow(tr("ID:"), mPD_Id);
    form->addRow(tr("Sex:"), mPD_Sex);
    form->addRow(tr("Birth:"), mPD_Birth);

    v->addWidget(body, 1);

    connect(dlg, &QObject::destroyed, this, [this] { mPatientDlg = nullptr; });
}

void MainWindow::onShowVolume3D()
{
    if (!mPlanar) return;

    auto vtkVol = mPlanar->makeVtkVolume();
    if (!vtkVol) {
        mStatusText->setText(tr("Warning"));
        return;
    }

    if (!mRenderView)
    {
        mRenderView = new RenderView(mViewerStack);
        mViewerStack->addWidget(mRenderView);
    }

    mTitle->setSaveVisible(true);

    mRenderView->setVolume(vtkVol, mPlanar->GetDicomInfo());
    mViewerStack->setCurrentWidget(mRenderView);

    if (mTitle) { mTitle->set3DChecked(true); mTitle->set2DChecked(false); }
    mStatusText->setText(tr("Ready Volume"));
}

void MainWindow::onShowPlanar2D()
{
    if (mPlanar) {
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

    ensurePatientDialog();

    mPD_Name->setText(mCurrentPatient.patientName);
    mPD_Id->setText(mCurrentPatient.patientId);
    mPD_Sex->setText(mCurrentPatient.sex);
    mPD_Birth->setText(mCurrentPatient.birthDate);

    mPatientDlg->show();
    mPatientDlg->raise();
    mPatientDlg->activateWindow();

    // Центрируем
    const QRect r = geometry();
    const QSize s = mPatientDlg->size();
    mPatientDlg->move(r.center() - QPoint(s.width() / 2, s.height() / 2 + 40));
}