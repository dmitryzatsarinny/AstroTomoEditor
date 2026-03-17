#include "DicomSeriesSaveDialog.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>

DicomSeriesSaveDialog::DicomSeriesSaveDialog(QWidget* parent)
    : DialogShell(parent, QObject::tr("Dicom Save"), WindowType::DicomSave)
{
    setWindowFlag(Qt::Tool);
    setFixedSize(mSize);

    QWidget* content = contentWidget();
    content->setObjectName("DicomSeriesSaveDialogContent");

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

    v->addLayout(mForm);

    content->setStyleSheet(
        "#DicomSeriesSaveDialogContent { background: transparent; }"
        "QLabel { color:#e6e6e6; }"
        "QLabel[role=\"label\"] { color:rgba(255,255,255,0.70); }"
    );

    retranslateUi();
}

void DicomSeriesSaveDialog::retranslateUi()
{
    DialogShell::retranslateUi();

    const QString title = tr("Dicom Save");
    setWindowTitle(title);
    if (titleBar())
        titleBar()->setTitle(title);
}