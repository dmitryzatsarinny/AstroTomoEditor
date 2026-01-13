#pragma once

#include "DialogShell.h"
#include <Services/Pool.h>    // тут лежит PatientInfo
#include <Services/PatientInfo.h>
#include <Window/MainWindow/TitleBar.h>

class QLabel;
class QFormLayout;

class PatientDialog : public DialogShell
{
    Q_OBJECT
public:
    explicit PatientDialog(QWidget* parent = nullptr);

    void setInfo(const PatientInfo& info);
    void retranslateUi();


private:
    QLabel* mName = nullptr;
    QLabel* mId = nullptr;
    QLabel* mSex = nullptr;
    QLabel* mBirth = nullptr;

    QLabel* mMode = nullptr;
    QLabel* mDescr = nullptr;
    QLabel* mSeq = nullptr;

    // MRI-specific
    QLabel* mRepetitionTime = nullptr;   // TR
    QLabel* mEchoTime = nullptr;         // TE
    QLabel* mInversionTime = nullptr;    // TI
    QLabel* mFlipAngle = nullptr;        // FA
    QLabel* mScanningSequence = nullptr; // ScanningSequence
    QLabel* mImageType = nullptr;        // ImageType

    QLabel* mMagneticFieldStrength = nullptr;
    QLabel* mScanOptions = nullptr;
    QLabel* mManufacturer = nullptr;
    QLabel* mContrastBolusAgent = nullptr;
    QLabel* mContrastBolusStartTime = nullptr;

    QFormLayout* mForm = nullptr;

    QString mSexCode;

    const QSize mCompactSize{ 420, 170 };     // только демография
    const QSize mSeriesSize{ 420, 220 };      // демография + поля серии
    const QSize mMriSeriesSize{ 420, 390 };   // демография + поля серии + MRI

    void setSeriesRowVisible(QLabel* field, bool visible);
    void setRowLabelText(QLabel* field, const QString& text);
};