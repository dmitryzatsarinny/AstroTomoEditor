#pragma once
#include "DialogShell.h"
#include "SeriesListPanel.h"
#include <QVector>
#include <QString>

class QCheckBox;
class QFormLayout;
class QPushButton;
class QVBoxLayout;

class DicomSeriesSaveDialog : public DialogShell
{
    Q_OBJECT
public:
    explicit DicomSeriesSaveDialog(QWidget* parent = nullptr);

    void retranslateUi();
    void setSeries(const QVector<SeriesExportEntry>& series);

signals:
    void saveRequested(const QVector<SeriesExportEntry>& selectedSeries);

private:
    struct RowWidgets
    {
        QCheckBox* check = nullptr;
        SeriesExportEntry* entry = nullptr;
    };

    void rebuildSeriesList();
    void updateSaveButtonState();

    QVector<SeriesExportEntry> mSeries;
    QVector<RowWidgets> mRows;

    QVBoxLayout* mContentLayout = nullptr;
    QFormLayout* mForm = nullptr;
    QPushButton* mSaveBtn = nullptr;

    const QSize mSize{ 520, 420 };
};