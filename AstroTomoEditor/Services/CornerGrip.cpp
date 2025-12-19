#include "CornerGrip.h"
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>

CornerGrip::CornerGrip(QWidget* parent) : QWidget(parent) {
    setFixedSize(22, 22);
    setCursor(Qt::SizeFDiagCursor); 
    setToolTip(tr("Change size"));
    setAttribute(Qt::WA_NoMousePropagation);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setStyleSheet("background: transparent;");
}

void CornerGrip::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    m_startPos = e->globalPosition().toPoint();
    if (auto* w = window()) m_startGeom = w->geometry();
    m_drag = true;
}

void CornerGrip::mouseMoveEvent(QMouseEvent* e) {
    if (!m_drag || !(e->buttons() & Qt::LeftButton)) return;
    const QPoint d = e->globalPosition().toPoint() - m_startPos;
    if (auto* w = window()) {
        const QSize min = w->minimumSize();
        const int wNew = std::max(min.width(), m_startGeom.width() + d.x());
        const int hNew = std::max(min.height(), m_startGeom.height() + d.y());
        w->resize(wNew, hNew);
    }
}

void CornerGrip::mouseReleaseEvent(QMouseEvent*) { m_drag = false; }

void CornerGrip::paintEvent(QPaintEvent*) {
    // рисуем «уголок» (три диагональные штриха)
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(230, 230, 230, 160), 1.2));
    for (int i = 1; i < 4; ++i)
        p.drawLine(width() - 1 - 6 * i, height() - 1, width() - 1, height() - 1 - 6 * i);
}
