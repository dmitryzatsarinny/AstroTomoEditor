#include "DicomSeriesSaveDialog.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
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
    scroll->setObjectName("DicomSeriesScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* scrollHost = new QWidget(scroll);
    scrollHost->setObjectName("DicomSeriesScrollHost");
    mContentLayout = new QVBoxLayout(scrollHost);
    mContentLayout->setContentsMargins(4, 4, 4, 4);
    mContentLayout->setSpacing(6);
    mContentLayout->addStretch(1);
    scroll->setWidget(scrollHost);
    outer->addWidget(scroll, 1);

    auto* buttonsRow = new QHBoxLayout();
    buttonsRow->setContentsMargins(0, 0, 0, 0);
    buttonsRow->setSpacing(8);
    buttonsRow->addStretch(1);

    mSaveToPatientBtn = new QPushButton(content);
    mSaveToPatientBtn->setObjectName("DicomSeriesSecondaryButton");
    mSaveToPatientBtn->setCursor(Qt::PointingHandCursor);
    buttonsRow->addWidget(mSaveToPatientBtn);

    mSaveBtn = new QPushButton(content);
    mSaveBtn->setObjectName("DicomSeriesSaveButton");
    mSaveBtn->setCursor(Qt::PointingHandCursor);
    buttonsRow->addWidget(mSaveBtn);
    outer->addLayout(buttonsRow);

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

    connect(mSaveToPatientBtn, &QPushButton::clicked, this, [this]()
        {
            emit saveToPatientRequested();
        });

    content->setStyleSheet(
        "#DicomSeriesSaveDialogContent { background: transparent; }"
        "#DicomSeriesHint { color:#d7dbe1; }"
        "#DicomSeriesScrollHost { background: transparent; }"
        "QScrollArea#DicomSeriesScroll { background: rgba(255,255,255,0.03); border: 1px solid rgba(255,255,255,0.12); border-radius: 8px; }"
        "QWidget#DicomSeriesRow { background: rgba(255,255,255,0.04); border: 1px solid rgba(255,255,255,0.08); border-radius: 8px; }"
        "QWidget#DicomSeriesRow:hover { background: rgba(255,255,255,0.08); border: 1px solid rgba(255,255,255,0.18); }"
        "QLabel#DicomSeriesThumb { background: rgba(0,0,0,0.35); border: 1px solid rgba(255,255,255,0.16); border-radius: 6px; }"
        "QCheckBox#DicomSeriesCheck { spacing: 10px; color:#f2f5f8; }"
        "QCheckBox#DicomSeriesCheck::indicator { width: 16px; height: 16px; border-radius: 3px; border: 1px solid rgba(255,255,255,0.32); background: rgba(0,0,0,0.28); }"
        "QCheckBox#DicomSeriesCheck::indicator:checked { background: #54a4ff; border: 1px solid #54a4ff; }"
        "QPushButton#DicomSeriesSaveButton, QPushButton#DicomSeriesSecondaryButton { min-height: 30px; border-radius: 6px; padding: 0 12px; font-weight: 600; }"
        "QPushButton#DicomSeriesSaveButton { background:#2f7de1; color:white; border:1px solid #4f95ec; }"
        "QPushButton#DicomSeriesSaveButton:hover { background:#3c8bec; }"
        "QPushButton#DicomSeriesSaveButton:disabled { background:rgba(255,255,255,0.10); color:rgba(255,255,255,0.45); border:1px solid rgba(255,255,255,0.14); }"
        "QPushButton#DicomSeriesSecondaryButton { background:rgba(255,255,255,0.08); color:#e8edf5; border:1px solid rgba(255,255,255,0.2); }"
        "QPushButton#DicomSeriesSecondaryButton:hover { background:rgba(255,255,255,0.14); }"
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
    while (mContentLayout->count() > 1)
    {
        QLayoutItem* item = mContentLayout->takeAt(0);
        if (!item)
            continue;

        if (QWidget* w = item->widget())
            w->deleteLater();

        delete item;
    }

    mRows.clear();

    QPixmap pixelIcon(32, 32);
    pixelIcon.fill(QColor(0, 0, 0, 0));
    {
        QPainter painter(&pixelIcon);
        painter.setPen(Qt::NoPen);
        const int px = 4;
        for (int y = 0; y < 8; ++y)
        {
            for (int x = 0; x < 8; ++x)
            {
                QColor c(65 + x * 12, 80 + y * 8, 175 + ((x + y) % 3) * 18, 230);
                painter.setBrush(c);
                painter.drawRect(x * px, y * px, px - 1, px - 1);
            }
        }
    }

    for (auto& entry : mSeries)
    {
        auto* row = new QWidget(this);
        row->setObjectName("DicomSeriesRow");

        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(10, 8, 10, 8);
        rowLayout->setSpacing(10);

        auto* thumb = new QLabel(row);
        thumb->setObjectName("DicomSeriesThumb");
        thumb->setFixedSize(32, 32);
        thumb->setPixmap(pixelIcon);
        rowLayout->addWidget(thumb, 0, Qt::AlignTop);

        auto* check = new QCheckBox(entry.description, row);
        check->setObjectName("DicomSeriesCheck");
        check->setChecked(true);
        rowLayout->addWidget(check, 1);

        mContentLayout->insertWidget(mContentLayout->count() - 1, row);
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
        mSaveBtn->setText(tr("Save selected"));

    if (mSaveToPatientBtn)
        mSaveToPatientBtn->setText(tr("SaveToPatient"));
}