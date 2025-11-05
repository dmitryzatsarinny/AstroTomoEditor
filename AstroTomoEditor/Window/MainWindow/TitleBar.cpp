#include "TitleBar.h"
#include <QHBoxLayout>
#include <QStyle>
#include <QMouseEvent>

TitleBar::TitleBar(QWidget* parent) : QWidget(parent)
{
    setFixedHeight(40);
    setObjectName("TitleBar");

    // Базовая сетка для ЛЕВО/ПРАВО (без центра!)
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(10, 0, 10, 0);
    grid->setHorizontalSpacing(8);

    // LEFT
    mLeft = new QWidget(this);
    auto* l = new QHBoxLayout(mLeft);
    l->setContentsMargins(0, 8, 0, 0);
    l->setSpacing(8);

    mIcon = new QLabel(mLeft);
    mIcon->setPixmap(QPixmap(":/icons/Resources/dicom_heart.png")
        .scaled(30, 30, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    l->addWidget(mIcon);

    mAppTitle = new QLabel(tr("AstroDicomEditor"), mLeft);
    mAppTitle->setStyleSheet("font-weight:600; font-size:20px;");
    l->addWidget(mAppTitle);

    mLeft->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    grid->addWidget(mLeft, 0, 0);

    // RIGHT
    mRight = new QWidget(this);
    auto* r = new QHBoxLayout(mRight);
    r->setContentsMargins(0, 8, 0, 0);
    r->setSpacing(6);

    mBtnSave3DR = new QToolButton(mRight);
    //mBtnSave3DR->setIcon(QIcon(":/icons/save.svg"));
    mBtnSave3DR->setObjectName("Save3DR");
    mBtnSave3DR->setToolTip(tr("Save 3DR (Ctrl+S)"));
    mBtnSave3DR->setText(tr("Save 3DR"));
    mBtnSave3DR->setCursor(Qt::PointingHandCursor);
    mBtnSave3DR->setFixedHeight(26);
    mBtnSave3DR->setVisible(false);
    auto pol = mBtnSave3DR->sizePolicy();
    pol.setRetainSizeWhenHidden(true);
    mBtnSave3DR->setSizePolicy(pol);

    // --- добавляем аккуратные кнопки 2D и 3D ---
    mBtn2D = new QToolButton(mRight);
    mBtn2D->setObjectName("Btn2D");
    mBtn2D->setText(tr("2D"));
    mBtn2D->setCursor(Qt::PointingHandCursor);
    mBtn2D->setFixedHeight(26);
    mBtn2D->setVisible(false);
    mBtn2D->setCheckable(true);
    pol = mBtn2D->sizePolicy();
    pol.setRetainSizeWhenHidden(true);
    mBtn2D->setSizePolicy(pol);
    
    mBtn3D = new QToolButton(mRight);
    mBtn3D->setObjectName("Btn3D");
    mBtn3D->setText(tr("3D"));
    mBtn3D->setCursor(Qt::PointingHandCursor);
    mBtn3D->setFixedHeight(26);
    mBtn3D->setVisible(false);
    mBtn3D->setCheckable(true);
    pol = mBtn3D->sizePolicy();
    pol.setRetainSizeWhenHidden(true);
    mBtn3D->setSizePolicy(pol);

    // эксклюзивная группа
    mViewGroup = new QButtonGroup(this);
    mViewGroup->setExclusive(true);
    mViewGroup->addButton(mBtn2D, 0);
    mViewGroup->addButton(mBtn3D, 1);
    mBtn2D->setChecked(true);

    // стиль «выбранной» кнопки
    const char* viewBtnStyle =
        "QToolButton#Btn2D, QToolButton#Btn3D, QToolButton#Save3DR {"
        "  color:#e6e6e6; background:transparent; border:none;"
        "  padding:2px 10px; font-size:16px; border-radius:6px;"
        "}"
        "QToolButton#Btn2D:hover, QToolButton#Btn3D:hover, QToolButton#Save3DR:hover {"
        "  background:rgba(255,255,255,0.10);"
        "}"
        "QToolButton#Btn2D:checked, QToolButton#Btn3D:checked {"
        "  background:rgba(255,255,255,0.18); color:#ffffff;"
        "}";

    mBtnSave3DR->setStyleSheet(viewBtnStyle);
    mBtn2D->setStyleSheet(viewBtnStyle);
    mBtn3D->setStyleSheet(viewBtnStyle);
    
    r->addWidget(mBtnSave3DR);
    r->addWidget(mBtn2D);
    r->addWidget(mBtn3D);

    // системные кнопки
    auto makeBtn = [&](QStyle::StandardPixmap sp) {
        auto* b = new QToolButton(this);
        b->setAutoRaise(true);
        b->setIcon(style()->standardIcon(sp));
        b->setFixedSize(52, 30);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
        };

    mBtnMax = makeBtn(QStyle::SP_TitleBarMaxButton);
    mBtnClose = makeBtn(QStyle::SP_TitleBarCloseButton);
    mBtnMax->setCursor(Qt::PointingHandCursor);
    mBtnClose->setCursor(Qt::PointingHandCursor);
    r->addWidget(mBtnMax);
    r->addWidget(mBtnClose);

    mRight->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    grid->addWidget(mRight, 0, 4);

    // Колонки: по центру оставим "пустые" растяжки
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 1); // пустая зона
    grid->setColumnStretch(2, 1); // пустая зона
    grid->setColumnStretch(3, 1); // пустая зона
    grid->setColumnStretch(4, 0);

    // === ЦЕНТР: оверлей на всю ширину, с центрированием кнопки ===
    mCenterOverlay = new QWidget(this);                 // важный момент: не в layout
    mCenterOverlay->setMouseTracking(true);
    mCenterOverlay->installEventFilter(this);

    auto* cLay = new QHBoxLayout(mCenterOverlay);
    cLay->setContentsMargins(0, 8, 0, 0);
    cLay->setSpacing(0);
    cLay->setAlignment(Qt::AlignCenter);

    mPatientBtn = new QToolButton(mCenterOverlay);
    mPatientBtn->setObjectName("PatientBtn");
    mPatientBtn->setAutoRaise(true);
    mPatientBtn->setCursor(Qt::PointingHandCursor);
    mPatientBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mPatientBtn->setMinimumHeight(24);
    mPatientBtn->setMaximumHeight(26);
    mPatientBtn->setStyleSheet(
        "QToolButton#PatientBtn{border:none;background:transparent;padding:0 8px;"
        "font-size:16px;color:#e6e6e6;} "
        "QToolButton#PatientBtn:hover{background:transparent;text-decoration:underline;}"
    );

    // IMPORTANT: кнопка должна получать события мыши
    // even though overlay is transparent for mouse, children still получают события
    cLay->addWidget(mPatientBtn);
    mCenterOverlay->raise(); // поверх

    // сигналы как были
    connect(mPatientBtn, &QToolButton::clicked, this, &TitleBar::patientClicked);
    connect(mBtn3D, &QToolButton::clicked, this, &TitleBar::volumeClicked);
    connect(mBtn2D, &QToolButton::clicked, this, &TitleBar::planarClicked);
    connect(mBtnMax, &QToolButton::clicked, this, [this] {
        if (!window()) return;
        window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
        updateMaximizeIcon();
        });
    connect(mBtnClose, &QToolButton::clicked, this, [this] {
        if (auto* w = window()) w->close();
        });

    connect(mBtnSave3DR, &QToolButton::clicked, this, &TitleBar::save3DRRequested);

    QTimer::singleShot(0, this, [this] { updateMaximizeIcon(); });
}

void TitleBar::updateOverlayGeometry()
{
    if (!mCenterOverlay) return;

    // текущие фактические ширины после раскладки
    const int lw = mLeft ? mLeft->width() : 0;
    const int rw = mRight ? mRight->width() : 0;

    const int h = height();
    int x = lw;
    int w = std::max(0, width() - lw - rw);

    // маленькая «страховка», чтобы оверлей не прилипал к кнопкам
    const int pad = 4;
    x += pad;
    w = std::max(0, w - 2 * pad);

    mCenterOverlay->setGeometry(x, 0, w, h);
}

void TitleBar::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    updateOverlayGeometry();
}

void TitleBar::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    QTimer::singleShot(0, this, [this] { updateOverlayGeometry(); });
}

// Фон оверлея: передаём те же действия перетаскивания окна
bool TitleBar::eventFilter(QObject* obj, QEvent* ev) {
    if (obj == mCenterOverlay) {
        switch (ev->type()) {
        case QEvent::MouseButtonPress: {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton && !window()->isMaximized()) {
                mDragging = true;
                mDragPos = me->globalPos() - window()->frameGeometry().topLeft();
                return true;
            }
            break;
        }
        case QEvent::MouseMove: {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (mDragging && (me->buttons() & Qt::LeftButton) && !window()->isMaximized()) {
                window()->move(me->globalPos() - mDragPos);
                return true;
            }
            break;
        }
        case QEvent::MouseButtonRelease:
            mDragging = false;
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void TitleBar::set2DVisible(bool on) {
    mBtn2D->setVisible(on);
    QTimer::singleShot(0, this, [this] { updateOverlayGeometry(); });
}

void TitleBar::set3DVisible(bool on) {
    mBtn3D->setVisible(on);
    QTimer::singleShot(0, this, [this] { updateOverlayGeometry(); });
}

void TitleBar::setSaveVisible(bool on) {
    mBtnSave3DR->setVisible(on);
    QTimer::singleShot(0, this, [this] { updateOverlayGeometry(); });
}

void TitleBar::set2DChecked(bool on)
{
    if (mBtn2D) mBtn2D->setChecked(on);
    if (on && mBtn3D) mBtn3D->setChecked(false);
}

void TitleBar::set3DChecked(bool on)
{
    if (mBtn3D) mBtn3D->setChecked(on);
    if (on && mBtn2D) mBtn2D->setChecked(false);
}

void TitleBar::setPatientInfo(const PatientInfo& info)
{
    mInfo = info;
    const QString line = info.patientName.isEmpty()
        ? tr("No Patient")
        : tr("%1  •  ID: %2  •  %3  •  %4")
        .arg(info.patientName, info.patientId, info.sex, info.birthDate);

    mPatientBtn->setText(line);

    // ширина по тексту + паддинги
    const QFontMetrics fm(mPatientBtn->font());
    const int w = fm.horizontalAdvance(line) + 16; // padding 8px слева/справа
    mPatientBtn->setFixedWidth(w);
}

bool TitleBar::isOverNonDraggableChild(const QPoint& pos) const
{
    QWidget* w = childAt(pos);
    return w == mPatientBtn || w == mBtnMax || w == mBtnClose;
}

void TitleBar::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton &&
        !isOverNonDraggableChild(e->pos()) &&
        !window()->isMaximized())             // ← добавили
    {
        mDragging = true;
        mDragPos = e->globalPos() - window()->frameGeometry().topLeft();
    }
    QWidget::mousePressEvent(e);
}

void TitleBar::mouseMoveEvent(QMouseEvent* e)
{
    if (mDragging && (e->buttons() & Qt::LeftButton) && !window()->isMaximized()) {
        window()->move(e->globalPos() - mDragPos);
    }
    QWidget::mouseMoveEvent(e);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && !isOverNonDraggableChild(e->pos())) {
        // двойной клик по заголовку — как в системе: переключить max/normal
        window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
        updateMaximizeIcon();
    }
    QWidget::mouseDoubleClickEvent(e);
}

void TitleBar::mouseReleaseEvent(QMouseEvent* e)
{
    mDragging = false;
    QWidget::mouseReleaseEvent(e);
}


void TitleBar::updateMaximizeIcon() {
    if (!mBtnMax) return;
    auto* w = window();
    if (!w || !style()) return;
    const bool maxed = w->isMaximized();
    mBtnMax->setIcon(style()->standardIcon(
        maxed ? QStyle::SP_TitleBarNormalButton : QStyle::SP_TitleBarMaxButton));
}