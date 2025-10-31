#include "TitleBar.h"
#include <QHBoxLayout>
#include <QStyle>
#include <QMouseEvent>

TitleBar::TitleBar(QWidget* parent) : QWidget(parent)
{
    setFixedHeight(44);
    setObjectName("TitleBar");

    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(10, 0, 10, 0);
    grid->setHorizontalSpacing(8);

    // LEFT
    auto* left = new QWidget(this);
    auto* l = new QHBoxLayout(left);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(8);

    mIcon = new QLabel(left);
    mIcon->setPixmap(QPixmap(":/icons/Resources/dicom_heart.png")
        .scaled(22, 22, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    l->addWidget(mIcon);

    mAppTitle = new QLabel(tr("AstroDicomEditor"), left);
    mAppTitle->setStyleSheet("font-weight:600; font-size:16px;");
    l->addWidget(mAppTitle);
    left->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    grid->addWidget(left, 0, 0);

    // CENTER — кнопка пациента
    auto* center = new QWidget(this);
    auto* c = new QHBoxLayout(center);
    c->setContentsMargins(0, 0, 0, 0);
    c->setSpacing(0);
    c->setAlignment(Qt::AlignCenter);

    mPatientBtn = new QToolButton(center);
    mPatientBtn->setObjectName("PatientBtn");
    mPatientBtn->setAutoRaise(true);
    mPatientBtn->setCursor(Qt::PointingHandCursor);
    mPatientBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mPatientBtn->setMinimumHeight(22);
    mPatientBtn->setMaximumHeight(24);
    mPatientBtn->setStyleSheet(
        "QToolButton#PatientBtn{border:none;background:transparent;padding:0 8px;"
        "font-size:14px;color:#e6e6e6;} "
        "QToolButton#PatientBtn:hover{background:transparent;text-decoration:underline;}");
    c->addWidget(mPatientBtn);
    center->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    grid->addWidget(center, 0, 2);

    // RIGHT — кнопки 2D/3D + системные
    auto* right = new QWidget(this);
    auto* r = new QHBoxLayout(right);
    r->setContentsMargins(0, 0, 0, 0);
    r->setSpacing(6);

    // --- добавляем аккуратные кнопки 2D и 3D ---
    mBtn2D = new QToolButton(right);
    mBtn2D->setObjectName("Btn2D");
    mBtn2D->setText(tr("2D"));
    mBtn2D->setCursor(Qt::PointingHandCursor);
    mBtn2D->setFixedHeight(24);
    mBtn2D->setVisible(false);
    mBtn2D->setCheckable(true);

    mBtn3D = new QToolButton(right);
    mBtn3D->setObjectName("Btn3D");
    mBtn3D->setText(tr("3D"));
    mBtn3D->setCursor(Qt::PointingHandCursor);
    mBtn3D->setFixedHeight(24);
    mBtn3D->setVisible(false);
    mBtn3D->setCheckable(true);

    // эксклюзивная группа
    mViewGroup = new QButtonGroup(this);
    mViewGroup->setExclusive(true);
    mViewGroup->addButton(mBtn2D, 0);
    mViewGroup->addButton(mBtn3D, 1);
    mBtn2D->setChecked(true); // по умолчанию 2D

    // стиль «выбранной» кнопки
    const char* viewBtnStyle =
        "QToolButton#Btn2D, QToolButton#Btn3D {"
        "  color:#e6e6e6; background:transparent; border:none;"
        "  padding:2px 10px; font-size:13px; border-radius:6px;"
        "}"
        "QToolButton#Btn2D:hover, QToolButton#Btn3D:hover {"
        "  background:rgba(255,255,255,0.10);"
        "}"
        "QToolButton#Btn2D:checked, QToolButton#Btn3D:checked {"
        "  background:rgba(255,255,255,0.18); color:#ffffff;"
        "}";

    mBtn2D->setStyleSheet(viewBtnStyle);
    mBtn3D->setStyleSheet(viewBtnStyle);

    r->addWidget(mBtn2D);
    r->addWidget(mBtn3D);

    // системные кнопки
    auto makeBtn = [&](QStyle::StandardPixmap sp) {
        auto* b = new QToolButton(this);
        b->setAutoRaise(true);
        b->setIcon(style()->standardIcon(sp));
        b->setFixedSize(30, 24);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
        };
    mBtnMax = makeBtn(QStyle::SP_TitleBarMaxButton);
    mBtnClose = makeBtn(QStyle::SP_TitleBarCloseButton);
    r->addWidget(mBtnMax);
    r->addWidget(mBtnClose);

    right->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    grid->addWidget(right, 0, 4);

    // колонки
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 0);
    grid->setColumnStretch(3, 1);
    grid->setColumnStretch(4, 0);

    // сигналы
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

    updateMaximizeIcon();
}

void TitleBar::set2DVisible(bool on)
{
    mBtn2D->setVisible(on);
}

void TitleBar::set3DVisible(bool on)
{
    mBtn3D->setVisible(on);
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
        ? tr("Пациент не выбран")
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


void TitleBar::updateMaximizeIcon()
{
    if (!window()) return;
    const bool maxed = window()->isMaximized();
    mBtnMax->setIcon(style()->standardIcon(maxed ? QStyle::SP_TitleBarNormalButton
        : QStyle::SP_TitleBarMaxButton));
}