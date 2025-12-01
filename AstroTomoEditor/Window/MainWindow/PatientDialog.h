#pragma once

#include "DialogShell.h"
#include <Services/Pool.h>    // тут лежит PatientInfo
#include <Services/PatientInfo.h>

class QLabel;

class PatientDialog : public DialogShell
{
    Q_OBJECT
public:
    explicit PatientDialog(QWidget* parent = nullptr);

    void setInfo(const PatientInfo& info);

private:
    QLabel* mName = nullptr;
    QLabel* mId = nullptr;
    QLabel* mSex = nullptr;
    QLabel* mBirth = nullptr;
};
