#pragma once
#pragma once
#include <QSpinBox>

class WheelSpinButton : public QSpinBox
{
    Q_OBJECT
public:
    explicit WheelSpinButton(QWidget* parent = nullptr);

    // Удобные сеттеры для шага
    void setWheelStep(int step) { m_wheelStep = step; }

protected:
    // Только колесо меняет значение; клики/клавиатура игнорируются
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    int m_wheelStep = 1;   // обычный шаг (по умолчанию 1)
    void applyDeltaSteps(int steps);
    void initStyle();
};
