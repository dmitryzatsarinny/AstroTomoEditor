#pragma once
#include <QWidget>

class CornerGrip : public QWidget {
    Q_OBJECT
public:
    explicit CornerGrip(QWidget* parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void paintEvent(QPaintEvent*) override;

private:
    QPoint m_startPos;
    QRect  m_startGeom;
    bool   m_drag = false;
};
