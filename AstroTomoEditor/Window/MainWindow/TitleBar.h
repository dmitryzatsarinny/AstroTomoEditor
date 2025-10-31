#pragma once
#ifndef TITLEBAR_h
#define TITLEBAR_h

#include "..\..\Services\Pool.h"
#include "..\..\Services\PatientInfo.h"

class TitleBar : public QWidget {
    Q_OBJECT
public:
    explicit TitleBar(QWidget* parent = nullptr);
    void setPatientInfo(const PatientInfo& info);
    const PatientInfo& info() const { return mInfo; }
    void set2DChecked(bool on);
    void set3DChecked(bool on);
    void set2DVisible(bool on);
    void set3DVisible(bool on);

signals:
    void patientClicked();
    void volumeClicked();
    void planarClicked();

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    bool isOverNonDraggableChild(const QPoint& pos) const;
    void updateMaximizeIcon();
    QPoint mDragPos;
    bool   mDragging = false;
    QLabel* mIcon = nullptr;
    QLabel* mAppTitle = nullptr;

    QToolButton* mBtn2D = nullptr;
    QToolButton* mBtn3D = nullptr;
    QButtonGroup* mViewGroup = nullptr;

    QToolButton* mPatientBtn = nullptr;
    QToolButton* mBtnMax = nullptr;
    QToolButton* mBtnClose = nullptr;
    PatientInfo mInfo;
};

#endif