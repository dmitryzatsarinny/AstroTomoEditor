#include "PatientDialog.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>

namespace {
    static void markAsRowLabel(QLabel* lbl)
    {
        if (!lbl) return;
        lbl->setProperty("role", "label");
    }
}

PatientDialog::PatientDialog(QWidget* parent)
    : DialogShell(parent, QObject::tr("Patient data"), WindowType::Patient)
{
    setWindowFlag(Qt::Tool);
    setFixedSize(mCompactSize);

    QWidget* content = contentWidget();
    content->setObjectName("PatientDialogContent");

    auto* v = new QVBoxLayout(content);
    v->setContentsMargins(16, 12, 16, 16);
    v->setSpacing(10);

    mForm = new QFormLayout();
    mForm->setContentsMargins(0, 0, 0, 0);
    mForm->setHorizontalSpacing(8);
    mForm->setVerticalSpacing(6);
    mForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    mForm->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    mForm->setLabelAlignment(Qt::AlignLeft);

    mName = new QLabel(content);
    mId = new QLabel(content);
    mSex = new QLabel(content);
    mBirth = new QLabel(content);

    QFont f = mName->font();
    f.setBold(true);
    mName->setFont(f);

    mForm->addRow(tr("Name:"), mName);
    mForm->addRow(tr("ID:"), mId);
    mForm->addRow(tr("Sex:"), mSex);
    mForm->addRow(tr("Birth:"), mBirth);

    mMode = new QLabel(content);
    mDescr = new QLabel(content);
    mSeq = new QLabel(content);

    mForm->addRow(tr("Type:"), mMode);
    mForm->addRow(tr("Description:"), mDescr);
    mForm->addRow(tr("Sequence:"), mSeq);

    // --- MRI fields ---
    mRepetitionTime = new QLabel(content);
    mEchoTime = new QLabel(content);
    mInversionTime = new QLabel(content);
    mFlipAngle = new QLabel(content);
    mScanningSequence = new QLabel(content);
    mImageType = new QLabel(content);

    mMagneticFieldStrength = new QLabel(content);
    mScanOptions = new QLabel(content);
    mManufacturer = new QLabel(content);
    mContrastBolusAgent = new QLabel(content);
    mContrastBolusStartTime = new QLabel(content);

    mForm->addRow(tr("TR:"), mRepetitionTime);
    mForm->addRow(tr("TE:"), mEchoTime);
    mForm->addRow(tr("TI:"), mInversionTime);
    mForm->addRow(tr("Flip angle:"), mFlipAngle);
    mForm->addRow(tr("Scan sequence:"), mScanningSequence);
    mForm->addRow(tr("Image type:"), mImageType);

    mForm->addRow(tr("B0:"), mMagneticFieldStrength);
    mForm->addRow(tr("Scan options:"), mScanOptions);
    mForm->addRow(tr("Manufacturer:"), mManufacturer);
    mForm->addRow(tr("Contrast agent:"), mContrastBolusAgent);
    mForm->addRow(tr("Contrast start time:"), mContrastBolusStartTime);

    // помечаем все лейблы-ярлыки для QSS (role="label")
    for (auto* w : content->findChildren<QWidget*>())
    {
        auto* lbl = qobject_cast<QLabel*>(w);
        if (!lbl) continue;
    }

    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mName)));
    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mId)));
    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mSex)));
    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mBirth)));
    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mMode)));
    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mDescr)));
    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mSeq)));

    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mRepetitionTime)));
    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mEchoTime)));
    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mInversionTime)));
    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mFlipAngle)));
    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mScanningSequence)));
    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mImageType)));

    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mMagneticFieldStrength)));
    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mScanOptions)));
    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mManufacturer)));
    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mContrastBolusAgent)));
    markAsRowLabel(qobject_cast<QLabel*>(mForm->labelForField(mContrastBolusStartTime)));

    v->addLayout(mForm);

    content->setStyleSheet(
        "#PatientDialogContent { background: transparent; }"
        "QLabel { color:#e6e6e6; }"
        "QLabel[role=\"label\"] { color:rgba(255,255,255,0.70); }"
    );

    // по умолчанию никаких серийных полей нет — скрываем
    setSeriesRowVisible(mMode, false);
    setSeriesRowVisible(mDescr, false);
    setSeriesRowVisible(mSeq, false);

    setSeriesRowVisible(mRepetitionTime, false);
    setSeriesRowVisible(mEchoTime, false);
    setSeriesRowVisible(mInversionTime, false);
    setSeriesRowVisible(mFlipAngle, false);
    setSeriesRowVisible(mScanningSequence, false);
    setSeriesRowVisible(mImageType, false);

    setSeriesRowVisible(mMagneticFieldStrength, false);
    setSeriesRowVisible(mScanOptions, false);
    setSeriesRowVisible(mManufacturer, false);
    setSeriesRowVisible(mContrastBolusAgent, false);
    setSeriesRowVisible(mContrastBolusStartTime, false);

    retranslateUi();
}

void PatientDialog::setSeriesRowVisible(QLabel* field, bool visible)
{
    if (!field || !mForm)
        return;

    if (QWidget* lbl = mForm->labelForField(field))
        lbl->setVisible(visible);

    field->setVisible(visible);
}

void PatientDialog::setRowLabelText(QLabel* field, const QString& text)
{
    if (!mForm || !field)
        return;

    if (auto* lbl = qobject_cast<QLabel*>(mForm->labelForField(field)))
        lbl->setText(text);
}

void PatientDialog::retranslateUi()
{
    DialogShell::retranslateUi();

    const QString title = tr("Patient data");
    setWindowTitle(title);
    if (titleBar())
        titleBar()->setTitle(title);

    setRowLabelText(mName, tr("Name:"));
    setRowLabelText(mId, tr("ID:"));
    setRowLabelText(mSex, tr("Sex:"));
    setRowLabelText(mBirth, tr("Birth:"));

    setRowLabelText(mMode, tr("Type:"));
    setRowLabelText(mDescr, tr("Description:"));
    setRowLabelText(mSeq, tr("Sequence:"));

    setRowLabelText(mRepetitionTime, tr("TR:"));
    setRowLabelText(mEchoTime, tr("TE:"));
    setRowLabelText(mInversionTime, tr("TI:"));
    setRowLabelText(mFlipAngle, tr("Flip angle:"));
    setRowLabelText(mScanningSequence, tr("Scan sequence:"));
    setRowLabelText(mImageType, tr("Image type:"));
}

void PatientDialog::setInfo(const PatientInfo& info)
{
    mName->setText(info.patientName);
    mId->setText(info.patientId);
    mSex->setText(info.sex);
    mBirth->setText(info.birthDate);

    const QString mode = info.Mode.trimmed();
    const QString descr = info.Description.trimmed();
    const QString seq = info.Sequence.trimmed();

    const bool hasMode = !mode.isEmpty();
    const bool hasDescr = !descr.isEmpty();
    const bool hasSeq = !seq.isEmpty();

    if (hasMode)  mMode->setText(mode);   else mMode->clear();
    if (hasDescr) mDescr->setText(descr); else mDescr->clear();
    if (hasSeq)   mSeq->setText(seq);     else mSeq->clear();

    setSeriesRowVisible(mMode, hasMode);
    setSeriesRowVisible(mDescr, hasDescr);
    setSeriesRowVisible(mSeq, hasSeq);

    const bool anySeries = hasMode || hasDescr || hasSeq;

    // --- MRI fields ---
    const bool isMR = (mode.toUpper() == "MR");

    const QString trv = info.RepetitionTime.trimmed();
    const QString tev = info.EchoTime.trimmed();
    const QString tiv = info.InversionTime.trimmed();
    const QString fav = info.FlipAngle.trimmed();
    const QString scs = info.ScanningSequence.trimmed();
    const QString itp = info.ImageType.trimmed();

    QString b0 = info.MagneticFieldStrength.trimmed();
    const QString so = info.ScanOptions.trimmed();
    const QString man = info.Manufacturer.trimmed();
    const QString cba = info.ContrastBolusAgent.trimmed();
    const QString cbt = info.ContrastBolusStartTime.trimmed();

    // чуть “очеловечим” B0
    if (!b0.isEmpty())
    {
        // если это число и нет 'T' — допишем
        const QString up = b0.toUpper();
        if (!up.contains('T')) {
            bool ok = false;
            b0.toDouble(&ok);
            if (ok) b0 += " T";
        }
    }

    const bool hasTR = isMR && !trv.isEmpty();
    const bool hasTE = isMR && !tev.isEmpty();
    const bool hasTI = isMR && !tiv.isEmpty();
    const bool hasFA = isMR && !fav.isEmpty();
    const bool hasSS = isMR && !scs.isEmpty();
    const bool hasIT = isMR && !itp.isEmpty();

    const bool hasB0 = isMR && !b0.isEmpty();
    const bool hasSO = isMR && !so.isEmpty();
    const bool hasMAN = isMR && !man.isEmpty();
    const bool hasCBA = !cba.isEmpty();
    const bool hasCBT = !cbt.isEmpty();

    if (hasTR)  mRepetitionTime->setText(trv); else mRepetitionTime->clear();
    if (hasTE)  mEchoTime->setText(tev);       else mEchoTime->clear();
    if (hasTI)  mInversionTime->setText(tiv);  else mInversionTime->clear();
    if (hasFA)  mFlipAngle->setText(fav);      else mFlipAngle->clear();
    if (hasSS)  mScanningSequence->setText(scs); else mScanningSequence->clear();
    if (hasIT)  mImageType->setText(itp);        else mImageType->clear();

    if (hasB0)  mMagneticFieldStrength->setText(b0); else mMagneticFieldStrength->clear();
    if (hasSO)  mScanOptions->setText(so);           else mScanOptions->clear();
    if (hasMAN) mManufacturer->setText(man);         else mManufacturer->clear();
    if (hasCBA) mContrastBolusAgent->setText(cba);   else mContrastBolusAgent->clear();
    if (hasCBT) mContrastBolusStartTime->setText(cbt); else mContrastBolusStartTime->clear();

    setSeriesRowVisible(mRepetitionTime, hasTR);
    setSeriesRowVisible(mEchoTime, hasTE);
    setSeriesRowVisible(mInversionTime, hasTI);
    setSeriesRowVisible(mFlipAngle, hasFA);
    setSeriesRowVisible(mScanningSequence, hasSS);
    setSeriesRowVisible(mImageType, hasIT);

    setSeriesRowVisible(mMagneticFieldStrength, hasB0);
    setSeriesRowVisible(mScanOptions, hasSO);
    setSeriesRowVisible(mManufacturer, hasMAN);
    setSeriesRowVisible(mContrastBolusAgent, hasCBA);
    setSeriesRowVisible(mContrastBolusStartTime, hasCBT);

    const bool anyMri =
        hasTR || hasTE || hasTI || hasFA || hasSS || hasIT ||
        hasB0 || hasSO || hasMAN;

    QSize targetSize =
        anyMri ? mMriSeriesSize :
        anySeries ? mSeriesSize :
        mCompactSize;

    if (hasCBA) targetSize.setHeight(targetSize.height() + 10);
    if (hasCBT) targetSize.setHeight(targetSize.height() + 10);

    setMinimumSize(targetSize);
    setMaximumSize(targetSize);
    resize(targetSize);
    updateGeometry();
}