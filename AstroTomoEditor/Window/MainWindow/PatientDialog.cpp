#include "PatientDialog.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>

PatientDialog::PatientDialog(QWidget* parent)
    : DialogShell(parent, QObject::tr("Patient Data"))
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
    mForm->setVerticalSpacing(4);
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
}

void PatientDialog::setSeriesRowVisible(QLabel* field, bool visible)
{
    if (!field || !mForm)
        return;

    if (QWidget* lbl = mForm->labelForField(field))
        lbl->setVisible(visible);

    field->setVisible(visible);
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
    const QSize targetSize = anySeries ? mExtendedSize : mCompactSize;

    setMinimumSize(targetSize);
    setMaximumSize(targetSize);
    resize(targetSize);
    updateGeometry();
}