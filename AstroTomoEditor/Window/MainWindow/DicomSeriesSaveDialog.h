#pragma once
#include "DialogShell.h"
#include "SeriesListPanel.h"
#include <QSet>
#include <QString>
#include <QVector>

struct PatientFolderEntry
{
    QString folderName;
    QString folderPath;
    QString patientName;
    bool hasCt = false;
};

class QCheckBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

class DicomSeriesSaveDialog : public DialogShell
{
    Q_OBJECT
public:
    explicit DicomSeriesSaveDialog(QWidget* parent = nullptr);

    void retranslateUi();
    void setSeries(const QVector<SeriesExportEntry>& series);
    void setSaveToPatientEnabled(bool enabled);
    void setPatients(const QVector<PatientFolderEntry>& patients);
    QVector<SeriesExportEntry> selectedSeries() const;
    QString selectedPatientFolder() const;

signals:
    void saveRequested(const QVector<SeriesExportEntry>& selectedSeries);
    void saveToPatientRequested(const QString& patientFolder, const QVector<SeriesExportEntry>& selectedSeries);
    void refreshPatientsRequested();

private:
    struct RowWidgets
    {
        QCheckBox* check = nullptr;
        SeriesExportEntry* entry = nullptr;
    };

    struct PatientRowWidgets
    {
        QCheckBox* check = nullptr;
        PatientFolderEntry* entry = nullptr;
    };

    enum class PageMode
    {
        SeriesSelection,
        PatientSelection
    };

    void rebuildSeriesList();
    void rebuildPatientList();
    void showSeriesPage();
    void showPatientPage();
    void updateSaveButtonState();

    QVector<SeriesExportEntry> mSeries;
    QVector<PatientFolderEntry> mPatients;
    QVector<RowWidgets> mRows;
    QVector<PatientRowWidgets> mPatientRows;
    QSet<QString> mCheckedSeriesKeys;
    QString mSelectedPatientFolder;
    bool mSaveToPatientEnabled = false;
    PageMode mPageMode = PageMode::SeriesSelection;

    QVBoxLayout* mContentLayout = nullptr;
    QLabel* mHintLabel = nullptr;
    QPushButton* mBackBtn = nullptr;
    QPushButton* mSaveBtn = nullptr;
    QPushButton* mSaveToPatientBtn = nullptr;
    QPushButton* mRefreshPatientsBtn = nullptr;

    const QSize mSize{ 620, 420 };
};