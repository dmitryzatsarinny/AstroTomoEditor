// TemplateDialog.h
#pragma once
#include <functional>
#include <unordered_map>
#include <vector>

#include <QDialog>
#include <QVector>
#include <QImage>
#include <Services/DicomRange.h>
#include "U8Span.h"
#include "..\MainWindow\DialogShell.h"
#include "..\MainWindow\TitleBar.h"

class QWidget;
class vtkImageData;

class QToolButton;
class QLabel;
class QGridLayout;
class QEvent;
class QObject;

struct DicomInfo;

enum class TemplateId : int
{
    LA = 0,
    RA,
    LV,
    RV,
    LA_Endo,
    RA_Endo,
    LV_Endo,
    RV_Endo,

    Heart,
    Body,
    Electrodes,
    Tpl1,
    Tpl2,
    Tpl3,

    Count
};


class TemplateDialog : public DialogShell {
    Q_OBJECT
public:
    explicit TemplateDialog(QWidget* parent, vtkImageData* image = nullptr, const DicomInfo* dinfo = nullptr);
    void setOnFinished(std::function<void()> cb) { m_onFinished = std::move(cb); }

    struct Slot {
        Volume data;
        bool visible = false;
        bool hasData() const { return data.raw(); }
    };

    // доступ из RenderView (без копий)
    const Slot* slot(TemplateId id) const;
    Slot* slot(TemplateId id);
    void loadAllTemplatesFromDisk(vtkImageData* mImage);
    QString templatesFolderPath() const;

signals:
    // ВАЖНО: отдаем ГРАНИЦЫ В ДАННЫХ HistMin..HistMax
    void requestCapture(TemplateId id);       // нажали Save
    void requestSetVisible(TemplateId id, bool on); // нажали Show/Hide или toggle
    void requestClear(TemplateId id);         // очистить слот
    void requestClearAll();                   // очистить все
    void requestClearScene();

public slots:
    void setCaptured(TemplateId id, Volume img);
    void onSaveAllTemplates(bool hide = false, bool saveto = false, const QString& savedir = {});
    void onClearScene();

protected:
    bool eventFilter(QObject* o, QEvent* e) override;
    void done(int r) override;
    void changeEvent(QEvent* e) override;

private:
    std::function<void()> m_onFinished;

    void buildUi();
    void retranslateUi();

    void setVisibleInternal(TemplateId id, bool on);
    void makeRow(QGridLayout* g, int row, TemplateId id, const QString& name);
    void refreshAll();

    int idxOf(TemplateId id);
    void applyTexts();
    QString safeSeriesFolderName(QString series) const;

    bool loadSlotFromMini3dr(TemplateId id, const QString& filePath, vtkImageData* mImage);

private:
    const DicomInfo* mDinfo = nullptr;
    // данные
    vtkImageData* mImage{ nullptr };
    const QSize mSize{ 420, 140 };

    struct Row {
        QToolButton* toggle = nullptr;
        QToolButton* save = nullptr;
        QToolButton* clear = nullptr;
        QLabel* state = nullptr;
        TemplateId id{};
    };

    std::unordered_map<TemplateId, Slot> mSlots;
    std::vector<Row> mRows;

    QLabel* mHdrLeft = nullptr;
    QLabel* mHdrRight = nullptr;

    struct ItemDesc { TemplateId id; const char* key; };
    static const ItemDesc kLeftItems[8];
    static const ItemDesc kRightItems[6];


    QToolButton* mBtnClrScene = nullptr;
    QToolButton* mBtnSaveAll = nullptr;

    QString templateFileStem(TemplateId id) const;     // имя файла по id
    bool saveSlotTo3dr(const QString& filePath, const Slot& s); // реальное сохранение
};
