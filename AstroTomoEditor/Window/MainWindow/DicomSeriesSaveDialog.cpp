#include "DicomSeriesSaveDialog.h"

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QVBoxLayout>
#include <functional>
#include "TitleBar.h"

namespace
{
    class ClickableSeriesRow : public QWidget
    {
    public:
        using QWidget::QWidget;

        std::function<void()> onClick;

    protected:
        void mousePressEvent(QMouseEvent* event) override
        {
            if (event->button() == Qt::LeftButton && onClick)
                onClick();

            QWidget::mousePressEvent(event);
        }
    };
}

DicomSeriesSaveDialog::DicomSeriesSaveDialog(QWidget* parent)
    : DialogShell(parent, QObject::tr("Save Dicom series"), WindowType::DicomSave)
{
    setWindowFlag(Qt::Tool);
    setFixedSize(mSize);

    QWidget* content = contentWidget();
    content->setObjectName("DicomSeriesSaveDialogContent");

    auto* outer = new QVBoxLayout(content);
    outer->setContentsMargins(16, 12, 16, 16);
    outer->setSpacing(10);

    mHintLabel = new QLabel(content);
    mHintLabel->setObjectName("DicomSeriesHint");
    mHintLabel->setWordWrap(true);
    outer->addWidget(mHintLabel);

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
    
    mBackBtn = new QPushButton(content);
    mBackBtn->setObjectName("DicomSeriesSecondaryButton");
    mBackBtn->setCursor(Qt::PointingHandCursor);
    buttonsRow->addWidget(mBackBtn);
    
    mRefreshPatientsBtn = new QPushButton(content);
    mRefreshPatientsBtn->setObjectName("DicomSeriesSecondaryButton");
    mRefreshPatientsBtn->setCursor(Qt::PointingHandCursor);
    buttonsRow->addWidget(mRefreshPatientsBtn);

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
            const QVector<SeriesExportEntry> selected = selectedSeries();

            if (selected.isEmpty())
                return;

            emit saveRequested(selected);
        });

    connect(mBackBtn, &QPushButton::clicked, this, [this]()
        {
            showSeriesPage();
        });

    connect(mRefreshPatientsBtn, &QPushButton::clicked, this, [this]()
        {
            emit refreshPatientsRequested();
        });

    connect(mSaveToPatientBtn, &QPushButton::clicked, this, [this]()
        {
            if (mPageMode == PageMode::SeriesSelection)
            {
                if (!mSaveToPatientEnabled || selectedSeries().isEmpty())
                    return;

                showPatientPage();
                return;
            }

            const QVector<SeriesExportEntry> selected = selectedSeries();
            const QString patientFolder = selectedPatientFolder();
            if (selected.isEmpty() || patientFolder.isEmpty())
                return;

            emit saveToPatientRequested(patientFolder, selected);
        });

    content->setStyleSheet(
        "#DicomSeriesSaveDialogContent { background: transparent; }"
        "#DicomSeriesHint { color:#d7dbe1; }"
        "#DicomSeriesScrollHost { background: transparent; }"
        "QScrollArea#DicomSeriesScroll { background: rgba(255,255,255,0.03); border: 1px solid rgba(255,255,255,0.12); border-radius: 8px; }"
        "QScrollBar:vertical { background: transparent; width: 8px; margin: 4px 0 4px 0; }"
        "QScrollBar::handle:vertical { background: rgba(255,255,255,0.18); min-height: 24px; border-radius: 4px; }"
        "QScrollBar::handle:vertical:hover { background: rgba(255,255,255,0.32); }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
        "QWidget#DicomSeriesRow, QWidget#DicomPatientRow { background: rgba(255,255,255,0.04); border: 1px solid rgba(255,255,255,0.08); border-radius: 8px; }"
        "QWidget#DicomSeriesRow:hover, QWidget#DicomPatientRow:hover { background: rgba(255,255,255,0.08); border: 1px solid rgba(255,255,255,0.18); }""QLabel#DicomSeriesThumb { background: rgba(0,0,0,0.35); border: 1px solid rgba(255,255,255,0.16); border-radius: 6px; }"
        "QCheckBox#DicomSeriesCheck, QCheckBox#DicomPatientCheck { spacing: 10px; color:#f2f5f8; }"
        "QCheckBox#DicomSeriesCheck::indicator, QCheckBox#DicomPatientCheck::indicator { width: 16px; height: 16px; border-radius: 3px; border: 1px solid rgba(255,255,255,0.32); background: rgba(0,0,0,0.28); }"
        "QCheckBox#DicomSeriesCheck::indicator:checked, QCheckBox#DicomPatientCheck::indicator:checked { background: #54a4ff; border: 1px solid #54a4ff; }"
        "QLabel#DicomPatientCtStatus[hasCt=\"true\"] { color:#7cd992; }"
        "QLabel#DicomPatientCtStatus[hasCt=\"false\"] { color:rgba(242,245,248,0.58); }"
        "QLabel#DicomPatientName { color:#f2f5f8; font-weight:600; }"
        "QLabel#DicomPatientFolder { color:rgba(242,245,248,0.72); }""QPushButton#DicomSeriesSaveButton, QPushButton#DicomSeriesSecondaryButton { min-height: 30px; border-radius: 6px; padding: 0 12px; font-weight: 600; }"
        "QPushButton#DicomSeriesSaveButton { background:#2f7de1; color:white; border:1px solid #4f95ec; }"
        "QPushButton#DicomSeriesSaveButton:hover { background:#3c8bec; }"
        "QPushButton#DicomSeriesSaveButton:disabled { background:rgba(255,255,255,0.10); color:rgba(255,255,255,0.45); border:1px solid rgba(255,255,255,0.14); }"
        "QPushButton#DicomSeriesSecondaryButton { background:rgba(255,255,255,0.08); color:#e8edf5; border:1px solid rgba(255,255,255,0.2); }"
        "QPushButton#DicomSeriesSecondaryButton:hover { background:rgba(255,255,255,0.14); }"
        "QPushButton#DicomSeriesSecondaryButton:disabled { background:rgba(255,255,255,0.05); color:rgba(255,255,255,0.35); border:1px solid rgba(255,255,255,0.10); }"
    );

    showSeriesPage();
    retranslateUi();
}

void DicomSeriesSaveDialog::setSeries(const QVector<SeriesExportEntry>& series)
{
    mSeries = series;
    QSet<QString> validKeys;
    for (const auto& entry : mSeries)
        validKeys.insert(entry.seriesKey);

    mCheckedSeriesKeys.intersect(validKeys);
    showSeriesPage();
}

void DicomSeriesSaveDialog::setSaveToPatientEnabled(bool enabled)
{
    mSaveToPatientEnabled = enabled;

    if (!enabled && mPageMode == PageMode::PatientSelection)
        showSeriesPage();

    updateSaveButtonState();
}

void DicomSeriesSaveDialog::setPatients(const QVector<PatientFolderEntry>& patients)
{
    mPatients = patients;

    bool selectedStillExists = false;
    for (const auto& patient : mPatients)
    {
        if (patient.folderPath == mSelectedPatientFolder)
        {
            selectedStillExists = true;
            break;
        }
    }

    if (!selectedStillExists)
        mSelectedPatientFolder.clear();

    if (mPageMode == PageMode::PatientSelection)
        rebuildPatientList();

    updateSaveButtonState();
}

QVector<SeriesExportEntry> DicomSeriesSaveDialog::selectedSeries() const
{
    QVector<SeriesExportEntry> selected;
    selected.reserve(mSeries.size());

    for (const auto& entry : mSeries)
    {
        if (mCheckedSeriesKeys.contains(entry.seriesKey))
            selected.push_back(entry);
    }

    return selected;
}

QString DicomSeriesSaveDialog::selectedPatientFolder() const
{
    return mSelectedPatientFolder;
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
    mPatientRows.clear();

    for (auto& entry : mSeries)
    {
        auto* row = new ClickableSeriesRow(this);
        row->setObjectName("DicomSeriesRow");
        row->setCursor(Qt::PointingHandCursor);

        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(10, 8, 10, 8);
        rowLayout->setSpacing(10);

        auto* check = new QCheckBox(row);
        check->setObjectName("DicomSeriesCheck");
        check->setChecked(mCheckedSeriesKeys.contains(entry.seriesKey));
        check->setCursor(Qt::PointingHandCursor);
        rowLayout->addWidget(check, 0, Qt::AlignVCenter);

        auto* thumb = new QLabel(row);
        thumb->setObjectName("DicomSeriesThumb");
        thumb->setFixedSize(32, 32);
        thumb->setAttribute(Qt::WA_TransparentForMouseEvents, true);

        QPixmap preview = entry.previewIcon.pixmap(64, 64);
        if (!preview.isNull())
            thumb->setPixmap(preview.scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation));

        rowLayout->addWidget(thumb, 0, Qt::AlignVCenter);

        auto* title = new QLabel(entry.description, row);
        title->setWordWrap(true);
        title->setStyleSheet("color:#f2f5f8;");
        title->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        rowLayout->addWidget(title, 1, Qt::AlignVCenter);

        row->onClick = [check]()
            {
                check->toggle();
            };

        connect(check, &QCheckBox::toggled, this, [this, key = entry.seriesKey](bool checked)
            {
                if (checked)
                    mCheckedSeriesKeys.insert(key);
                else
                    mCheckedSeriesKeys.remove(key);

                updateSaveButtonState();
            });

        mContentLayout->insertWidget(mContentLayout->count() - 1, row);
        mRows.push_back({ check, &entry });

    }

    updateSaveButtonState();
}

void DicomSeriesSaveDialog::rebuildPatientList()
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
    mPatientRows.clear();

    for (auto& patient : mPatients)
    {
        auto* row = new ClickableSeriesRow(this);
        row->setObjectName("DicomPatientRow");
        row->setCursor(Qt::PointingHandCursor);

        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(10, 10, 10, 10);
        rowLayout->setSpacing(10);

        auto* check = new QCheckBox(row);
        check->setObjectName("DicomPatientCheck");
        check->setChecked(patient.folderPath == mSelectedPatientFolder);
        check->setCursor(Qt::PointingHandCursor);
        rowLayout->addWidget(check, 0, Qt::AlignTop);

        auto* textHost = new QWidget(row);
        auto* textLayout = new QVBoxLayout(textHost);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(2);

        auto* nameLabel = new QLabel(patient.patientName.isEmpty() ? patient.folderName : patient.patientName, textHost);
        nameLabel->setObjectName("DicomPatientName");
        nameLabel->setWordWrap(true);
        nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        textLayout->addWidget(nameLabel);

        auto* folderLabel = new QLabel(patient.folderName, textHost);
        folderLabel->setObjectName("DicomPatientFolder");
        folderLabel->setWordWrap(true);
        folderLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        textLayout->addWidget(folderLabel);

        auto* ctLabel = new QLabel(patient.hasCt
            ? tr("CT folder already exists")
            : tr("CT folder not found"), textHost);
        ctLabel->setObjectName("DicomPatientCtStatus");
        ctLabel->setProperty("hasCt", patient.hasCt);
        ctLabel->setWordWrap(true);
        ctLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        textLayout->addWidget(ctLabel);

        rowLayout->addWidget(textHost, 1);

        row->onClick = [check]()
            {
                check->setChecked(true);
            };

        connect(check, &QCheckBox::toggled, this, [this, check, path = patient.folderPath](bool checked)
            {
                if (checked)
                {
                    mSelectedPatientFolder = path;
                    for (const auto& other : mPatientRows)
                    {
                        if (!other.check || other.check == check)
                            continue;

                        QSignalBlocker blocker(other.check);
                        other.check->setChecked(false);
                    }
                }
                else if (mSelectedPatientFolder == path)
                {
                    mSelectedPatientFolder.clear();
                }

                updateSaveButtonState();
            });

        mContentLayout->insertWidget(mContentLayout->count() - 1, row);
        mPatientRows.push_back({ check, &patient });
    }

    updateSaveButtonState();
}

void DicomSeriesSaveDialog::showSeriesPage()
{
    mPageMode = PageMode::SeriesSelection;
    rebuildSeriesList();
    updateSaveButtonState();
}

void DicomSeriesSaveDialog::showPatientPage()
{
    if (!mSaveToPatientEnabled || selectedSeries().isEmpty() || mPatients.isEmpty())
        return;

    mPageMode = PageMode::PatientSelection;
    rebuildPatientList();
    updateSaveButtonState();
}

void DicomSeriesSaveDialog::updateSaveButtonState()
{
    const bool anySeriesChecked = !selectedSeries().isEmpty();
    const bool hasPatients = !mPatients.isEmpty();
    const bool hasSelectedPatient = !mSelectedPatientFolder.isEmpty();

    if (mHintLabel)
    {
        if (mPageMode == PageMode::SeriesSelection)
            mHintLabel->setText(tr("Select the CT series to save"));
        else if (hasPatients)
            mHintLabel->setText(tr("Select the patient to receive the selected CT series"));
        else
            mHintLabel->setText(tr("No patients were found in the configured HDBASE folder"));
    }

    if (mBackBtn)
        mBackBtn->setVisible(mPageMode == PageMode::PatientSelection);

    if (mRefreshPatientsBtn)
    {
        mRefreshPatientsBtn->setVisible(mSaveToPatientEnabled);
        mRefreshPatientsBtn->setEnabled(mSaveToPatientEnabled);
    }

    if (mSaveBtn)
    {
        mSaveBtn->setVisible(mPageMode == PageMode::SeriesSelection);
        mSaveBtn->setEnabled(anySeriesChecked);
    }

    if (mSaveToPatientBtn)
    {
        mSaveToPatientBtn->setVisible(mSaveToPatientEnabled);
        if (mPageMode == PageMode::SeriesSelection)
            mSaveToPatientBtn->setEnabled(mSaveToPatientEnabled && anySeriesChecked && hasPatients);
        else
            mSaveToPatientBtn->setEnabled(mSaveToPatientEnabled && anySeriesChecked && hasSelectedPatient);
    }
}

void DicomSeriesSaveDialog::retranslateUi()
{
    DialogShell::retranslateUi();

    const QString title = tr("Save Dicom series");
    setWindowTitle(title);
    if (titleBar())
        titleBar()->setTitle(title);

    if (mBackBtn)
        mBackBtn->setText(tr("Back"));

    if (mSaveBtn)
        mSaveBtn->setText(tr("Save selected"));

    if (mSaveToPatientBtn)
        mSaveToPatientBtn->setText(tr("Save to patient"));
    
    if (mRefreshPatientsBtn)
        mRefreshPatientsBtn->setText(tr("Refresh patient list"));

    if (mPageMode == PageMode::PatientSelection)
        rebuildPatientList();

    updateSaveButtonState();
}