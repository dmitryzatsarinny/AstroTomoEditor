#include "WheelSpinButton.h"
#include <QWheelEvent>
#include <QLineEdit>
#include <QtMath>

WheelSpinButton::WheelSpinButton(QWidget* parent)
    : QSpinBox(parent)
{
    setButtonSymbols(QAbstractSpinBox::NoButtons);
    setAccelerated(true);

    setFocusPolicy(Qt::NoFocus);
    setContextMenuPolicy(Qt::NoContextMenu);

    if (auto* le = this->findChild<QLineEdit*>("qt_spinbox_lineedit")) {
        le->setReadOnly(true);
        le->setFocusPolicy(Qt::NoFocus);
        le->setContextMenuPolicy(Qt::NoContextMenu);
        le->setAttribute(Qt::WA_TransparentForMouseEvents, true); // мышь проходит мимо
        le->setCursor(Qt::ArrowCursor);
        le->deselect();
    }

    setAlignment(Qt::AlignCenter);
    setFrame(false);
    setFixedSize(44, 26);
    setCursor(Qt::PointingHandCursor);

    initStyle();
}



void WheelSpinButton::initStyle()
{
    setStyleSheet(
        "QSpinBox {"
        "  color:#fff; background:rgba(40,40,40,110);"
        "  border:1px solid rgba(255,255,255,30); border-radius:6px;"
        "  padding-top:-1px; font-size:13px; qproperty-alignment:'AlignCenter'; }"
        "QSpinBox:hover { background:rgba(255,255,255,40); }"
        "QSpinBox QLineEdit { background:transparent; border:none; padding:0; margin:0;"
        "  selection-background-color:transparent; selection-color:#fff; }"
        "QSpinBox::up-button, QSpinBox::down-button { width:0; height:0; border:none; }"
    );
}
void WheelSpinButton::applyDeltaSteps(int steps)
{
    if (steps == 0) return;

    const int step = m_wheelStep;

    // Корректно учитываем диапазон
    int v = value();
    const int newV = std::clamp(v + steps * step, minimum(), maximum());
    if (newV != v) setValue(newV);
}

void WheelSpinButton::wheelEvent(QWheelEvent* e)
{
    // Колесо работает ТОЛЬКО над виджетом — независимо от фокуса
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    const QPoint numDeg = e->angleDelta() / 120;     // дискретные «щёлчки»
    int steps = numDeg.y();                          // вертикальная прокрутка
#else
    int steps = e->delta() / 120;
#endif
    // Поддержим high-res сенсорные тачпады (pixelDelta): аккумулировать нет нужды — переводим в шаги
    if (steps == 0 && !e->pixelDelta().isNull()) {
        // эвристика: 120 пикселей ≈ 1 шаг (как 120 «угловых»)
        steps = qRound(double(e->pixelDelta().y()) / 120.0);
    }

    if (rect().contains(e->position().toPoint())) {
        applyDeltaSteps(steps);
        e->accept();      // не отдаём скролл родителю
        return;
    }
    // если колесо не над нами — поведение по умолчанию (передастся родителю)
    e->ignore();
}

void WheelSpinButton::mousePressEvent(QMouseEvent* e)
{
    // Клики не меняют значение и не дают фокус
    e->ignore();
}

void WheelSpinButton::mouseDoubleClickEvent(QMouseEvent* e)
{
    e->ignore();
}

void WheelSpinButton::keyPressEvent(QKeyEvent* e)
{
    // Полная блокировка клавиатурного управления
    e->ignore();
}

void WheelSpinButton::contextMenuEvent(QContextMenuEvent* e)
{
    // Без контекстного меню
    e->ignore();
}
