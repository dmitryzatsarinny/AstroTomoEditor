#include "DicomSeriesSaveDialog.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include "TitleBar.h"

DicomSeriesSaveDialog::DicomSeriesSaveDialog(QWidget* parent)
    : DialogShell(parent, QObject::tr("Dicom Save"), WindowType::DicomSave)
{
    setWindowFlag(Qt::Tool);
    setFixedSize(mSize);

    QWidget* content = contentWidget();
    content->setObjectName("DicomSeriesSaveDialogContent");

    auto* outer = new QVBoxLayout(content);
    outer->setContentsMargins(16, 12, 16, 16);
    outer->setSpacing(10);

    auto* hintLabel = new QLabel(content);
    hintLabel->setObjectName("DicomSeriesHint");
    hintLabel->setWordWrap(true);
    outer->addWidget(hintLabel);

    auto* scroll = new QScrollArea(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* scrollHost = new QWidget(scroll);
    mContentLayout = new QVBoxLayout(scrollHost);
    mContentLayout->setContentsMargins(0, 0, 0, 0);
    mContentLayout->setSpacing(8);

    mForm = new QFormLayout();
    mForm->setContentsMargins(0, 0, 0, 0);
    mForm->setHorizontalSpacing(8);
    mForm->setVerticalSpacing(6);
    mForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    mForm->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);

    mContentLayout->addLayout(mForm);
    mContentLayout->addStretch(1);
    scrollHost->setLayout(mContentLayout);
    scroll->setWidget(scrollHost);
    outer->addWidget(scroll, 1);

    mSaveBtn = new QPushButton(content);
    mSaveBtn->setObjectName("DicomSeriesSaveButton");
    mSaveBtn->setCursor(Qt::PointingHandCursor);
    outer->addWidget(mSaveBtn, 0, Qt::AlignRight);

    connect(mSaveBtn, &QPushButton::clicked, this, [this]()
        {
            QVector<SeriesExportEntry> selected;
            for (const auto& row : mRows)
            {
                if (!row.check || !row.entry)
                    continue;
                if (row.check->isChecked())
                    selected.push_back(*row.entry);
            }

            if (selected.isEmpty())
                return;

            emit saveRequested(selected);
        });

    content->setStyleSheet(
        "#DicomSeriesSaveDialogContent { background: transparent; }"
        "QLabel { color:#e6e6e6; }"
        "QLabel[role=\"label\"] { color:rgba(255,255,255,0.70); }"
    );

    retranslateUi();
}

void DicomSeriesSaveDialog::setSeries(const QVector<SeriesExportEntry>& series)
{
    mSeries = series;
    rebuildSeriesList();
}

void DicomSeriesSaveDialog::rebuildSeriesList()
{
    while (mForm->rowCount() > 0)
        mForm->removeRow(0);

    mRows.clear();

    for (auto& entry : mSeries)
    {
        auto* check = new QCheckBox(entry.description, this);
        check->setChecked(true);

        QLabel* lbl = new QLabel(tr("Series:"), this);
        lbl->setProperty("role", "label");

        mForm->addRow(lbl, check);
        mRows.push_back({ check, &entry });

        connect(check, &QCheckBox::toggled, this, [this]() { updateSaveButtonState(); });
    }

    updateSaveButtonState();
}

void DicomSeriesSaveDialog::updateSaveButtonState()
{
    bool anyChecked = false;
    for (const auto& row : mRows)
    {
        if (row.check && row.check->isChecked())
        {
            anyChecked = true;
            break;
        }
    }

    if (mSaveBtn)
        mSaveBtn->setEnabled(anyChecked);
}

void DicomSeriesSaveDialog::retranslateUi()
{
    DialogShell::retranslateUi();

    const QString title = tr("Dicom Save");
    setWindowTitle(title);
    if (titleBar())
        titleBar()->setTitle(title);

    if (auto* hint = contentWidget()->findChild<QLabel*>("DicomSeriesHint"))
        hint->setText(tr("Select the series to save."));

    if (mSaveBtn)
        mSaveBtn->setText(tr("Save selected series"));

    for (int i = 0; i < mForm->rowCount(); ++i)
    {
        if (auto* w = mForm->itemAt(i, QFormLayout::LabelRole))
            if (auto* lbl = qobject_cast<QLabel*>(w->widget()))
                lbl->setText(tr("Series:"));
    }
}