#include "PatientDialog.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>

PatientDialog::PatientDialog(QWidget* parent)
    : DialogShell(parent, QObject::tr("Patient Data"))
{
    setWindowFlag(Qt::Tool);
    setFixedSize(420, 160);

    QWidget* content = contentWidget();
    content->setObjectName("PatientDialogContent");

    auto* v = new QVBoxLayout(content);
    v->setContentsMargins(16, 12, 16, 16);
    v->setSpacing(10);

    auto* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(4);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setLabelAlignment(Qt::AlignLeft);


    mName = new QLabel(content);
    mId = new QLabel(content);
    mSex = new QLabel(content);
    mBirth = new QLabel(content);

    // делаем имя пациента полужирным
    QFont f = mName->font();
    f.setBold(true);
    mName->setFont(f);

    form->addRow(tr("Name:"), mName);
    form->addRow(tr("ID:"), mId);
    form->addRow(tr("Sex:"), mSex);
    form->addRow(tr("Birth:"), mBirth);

    v->addLayout(form);

    // Стиль
    content->setStyleSheet(
        "#PatientDialogContent { background: transparent; }"
        "QLabel { color:#e6e6e6; }"
        "QLabel[role=\"label\"] { color:rgba(255,255,255,0.70); }"
    );
}


void PatientDialog::setInfo(const PatientInfo& info)
{
    mName->setText(info.patientName);
    mId->setText(info.patientId);
    mSex->setText(info.sex);
    mBirth->setText(info.birthDate);
}
