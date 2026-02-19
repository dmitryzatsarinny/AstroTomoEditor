#include "TemplateDialog.h"

#include <QToolButton>
#include <QLabel>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QEvent>
#include <QFileDialog>
#include <QDir>
#include <QStandardPaths>
#include <QSettings>

#include "../../Services/Save3DR.h"
#include <Window/ServiceWindow/CustomMessageBox.h>

const TemplateDialog::ItemDesc TemplateDialog::kLeftItems[8] = {
    {TemplateId::LA,      QT_TR_NOOP("Left atrium")},
    {TemplateId::RA,      QT_TR_NOOP("Right atrium")},
    {TemplateId::LV,      QT_TR_NOOP("Left ventricle")},
    {TemplateId::RV,      QT_TR_NOOP("Right ventricle")},
    {TemplateId::LA_Endo, QT_TR_NOOP("LA endo")},
    {TemplateId::RA_Endo, QT_TR_NOOP("RA endo")},
    {TemplateId::LV_Endo, QT_TR_NOOP("LV endo")},
    {TemplateId::RV_Endo, QT_TR_NOOP("RV endo")},
};

const TemplateDialog::ItemDesc TemplateDialog::kRightItems[6] = {
    {TemplateId::Heart,      QT_TR_NOOP("Heart")},
    {TemplateId::Body,       QT_TR_NOOP("Body")},
    {TemplateId::Electrodes, QT_TR_NOOP("Electrodes")},
    {TemplateId::Tpl1,       QT_TR_NOOP("Template 1")},
    {TemplateId::Tpl2,       QT_TR_NOOP("Template 2")},
    {TemplateId::Tpl3,       QT_TR_NOOP("Template 3")},
};

QString TemplateDialog::templateFileStem(TemplateId id) const
{
    switch (id)
    {
    case TemplateId::LA:        return "LA";
    case TemplateId::RA:        return "RA";
    case TemplateId::LV:        return "LV";
    case TemplateId::RV:        return "RV";
    case TemplateId::LA_Endo:   return "LA_endo";
    case TemplateId::RA_Endo:   return "RA_endo";
    case TemplateId::LV_Endo:   return "LV_endo";
    case TemplateId::RV_Endo:   return "RV_endo";
    case TemplateId::Heart:     return "Heart";
    case TemplateId::Body:      return "Body";
    case TemplateId::Electrodes:return "Electrodes";
    case TemplateId::Tpl1:      return "Template1";
    case TemplateId::Tpl2:      return "Template2";
    case TemplateId::Tpl3:      return "Template3";
    default:                    return "Template";
    }
}

int TemplateDialog::idxOf(TemplateId id)
{
    return static_cast<int>(id);
}

static void setState(QLabel* st, bool has, bool vis)
{
    if (!st) return;

    st->setTextFormat(Qt::PlainText);
    st->setAlignment(Qt::AlignCenter);
    st->setFixedHeight(22);
    st->setMinimumWidth(64);

    if (!has) {
        st->setText(QObject::tr("empty"));
        st->setProperty("state", "empty");
        st->setStyleSheet("");
        st->style()->unpolish(st);
        st->style()->polish(st);
        return;
    }

    st->setText(vis ? QObject::tr("on") : QObject::tr("off"));
    st->setProperty("state", vis ? "on" : "off");
    st->setStyleSheet("");
    st->style()->unpolish(st);
    st->style()->polish(st);
}


void TemplateDialog::makeRow(QGridLayout* g, int row, TemplateId id, const QString& name)
{
    auto* btn = new QToolButton(contentWidget());
    btn->setObjectName("TplToggle");
    btn->setText(name);
    btn->setCheckable(true);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFixedHeight(34);
    btn->setToolButtonStyle(Qt::ToolButtonTextOnly);

    auto* save = new QToolButton(contentWidget());
    save->setObjectName("TplSave");
    save->setIcon(QIcon(":/icons/Resources/diskette-save.svg"));
    save->setIconSize(QSize(18, 18));
    save->setText("");
    save->setToolButtonStyle(Qt::ToolButtonIconOnly);
    save->setCursor(Qt::PointingHandCursor);
    save->setFixedHeight(34);
    save->setToolTip(tr("Save current visible volume"));

    auto* st = new QLabel(contentWidget());
    st->setObjectName("TplState");
    st->setText("empty");
    st->setAlignment(Qt::AlignCenter);
    st->setFixedHeight(22);

    mRows[idxOf(id)] = Row{ btn, save, nullptr, st, id };

    // сетка: [toggle][save][state]
    g->addWidget(btn, row, 0);
    g->addWidget(save, row, 1);
    g->addWidget(st, row, 2);

    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    save->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    st->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    connect(save, &QToolButton::clicked, this, [this, id] { emit requestCapture(id); });

    connect(btn, &QToolButton::toggled, this, [this, id](bool on) {
        auto& s = mSlots[id];

        if (on && !s.hasData())
        {
            auto& r = mRows[idxOf(id)];
            if (r.toggle) {
                r.toggle->blockSignals(true);
                r.toggle->setChecked(false);
                r.toggle->blockSignals(false);
            }
            return;
        }

        s.visible = on;
        emit requestSetVisible(id, on);
        refreshAll();
        });

    btn->setEnabled(false);
}


void TemplateDialog::refreshAll()
{
    for (auto& r : mRows)
    {
        if (!r.toggle || !r.save || !r.state) continue;

        const auto it = mSlots.find(r.id);
        const bool has = (it != mSlots.end() && it->second.hasData());
        const bool vis = (it != mSlots.end() && it->second.visible);

        r.toggle->setEnabled(has);
        r.toggle->blockSignals(true);
        r.toggle->setChecked(has && vis);
        r.toggle->blockSignals(false);

        setState(r.state, has, vis);
    }
}



TemplateDialog::TemplateDialog(QWidget* parent, vtkImageData* image, const DicomInfo* dinfo)
    : DialogShell(parent, tr("Templates"), WindowType::Template)
    , mImage(image)
    , mDinfo(dinfo)
{
    resize(860, 460);
    buildUi();
    refreshAll();
}

void TemplateDialog::buildUi()
{
    QWidget* content = contentWidget();
    content->setObjectName("TemplatesContent");

    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(16, 14, 16, 16);
    root->setSpacing(12);

    auto* cols = new QHBoxLayout();
    cols->setSpacing(14);
    cols->setContentsMargins(0, 0, 0, 0);
    root->addLayout(cols, 1);

    struct PanelPack { QWidget* panel; QVBoxLayout* v; QGridLayout* g; QLabel* header; };

    auto makePanel = [&](const char* headerKey) -> PanelPack
        {
            auto* panel = new QWidget(content);
            panel->setObjectName("TplPanel");

            auto* v = new QVBoxLayout(panel);
            v->setContentsMargins(12, 12, 12, 12);
            v->setSpacing(10);

            auto* header = new QLabel(tr(headerKey), panel);
            header->setObjectName("TplPanelHeader");
            v->addWidget(header);

            auto* g = new QGridLayout();
            g->setHorizontalSpacing(10);
            g->setVerticalSpacing(10);
            g->setContentsMargins(0, 0, 0, 0);

            g->setColumnStretch(0, 2);
            g->setColumnStretch(1, 1);
            g->setColumnStretch(2, 1);

            g->setColumnMinimumWidth(1, 56);
            g->setColumnMinimumWidth(2, 72);

            v->addLayout(g);
            v->addStretch(1);

            return { panel, v, g, header };
        };

    auto leftPack = makePanel(QT_TR_NOOP("Chambers"));
    auto rightPack = makePanel(QT_TR_NOOP("Other"));

    mHdrLeft = leftPack.header;
    mHdrRight = rightPack.header;

    cols->addWidget(leftPack.panel, 1);
    cols->addWidget(rightPack.panel, 1);

    // алиасы
    QGridLayout* left = leftPack.g;
    QGridLayout* right = rightPack.g;

    mRows.clear();
    mRows.resize(static_cast<int>(TemplateId::Count));

    for (int i = 0; i < 8; ++i)
        makeRow(left, i, kLeftItems[i].id, tr(kLeftItems[i].key));

    for (int i = 0; i < 6; ++i)
        makeRow(right, i, kRightItems[i].id, tr(kRightItems[i].key));

    mBtnClrScene = new QToolButton(rightPack.panel);
    mBtnClrScene->setObjectName("TplSaveAll");
    mBtnClrScene->setCursor(Qt::PointingHandCursor);
    mBtnClrScene->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mBtnClrScene->setFixedHeight(34);
    mBtnClrScene->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mBtnClrScene->setText(tr("Clear scene"));

    rightPack.v->addWidget(mBtnClrScene);

    connect(mBtnClrScene, &QToolButton::clicked, this, [this] {onClearScene(); });

    mBtnSaveAll = new QToolButton(rightPack.panel);
    mBtnSaveAll->setObjectName("TplSaveAll");
    mBtnSaveAll->setCursor(Qt::PointingHandCursor);
    mBtnSaveAll->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mBtnSaveAll->setFixedHeight(34);
    mBtnSaveAll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mBtnSaveAll->setText(tr("Save templates…"));

    rightPack.v->addWidget(mBtnSaveAll);

    connect(mBtnSaveAll, &QToolButton::clicked, this, [this] {onSaveAllTemplates();});

    // стили
    content->setStyleSheet(
        "#TemplatesContent { background: transparent; }"

        "#TplTitle { color:#f2f2f2; font-size:18px; font-weight:700; }"
        "#TplSubtitle { color:rgba(255,255,255,0.55); margin-bottom:2px; }"

        "#TplPanel {"
        "  background:rgba(30,32,36,210);"
        "  border:1px solid rgba(255,255,255,0.10);"
        "  border-radius:12px;"
        "}"
        "#TplPanelHeader {"
        "  color:rgba(255,255,255,0.85);"
        "  font-size:13px;"
        "  font-weight:600;"
        "}"

        "QToolButton#TplToggle {"
        "  padding:7px 12px;"
        "  background:rgba(42,44,48,220);"
        "  color:#f0f0f0;"
        "  border-radius:10px;"
        "  border:1px solid rgba(255,255,255,0.14);"
        "  text-align:left;"
        "}"
        "QToolButton#TplToggle:hover { background:rgba(60,63,70,220); border:1px solid rgba(255,255,255,0.20); }"
        "QToolButton#TplToggle:checked {"
        "  background:rgba(0,180,100,130);"
        "  border:1px solid rgba(0,180,100,200);"
        "}"
        "QToolButton#TplToggle:disabled { color:rgba(255,255,255,0.35); background:rgba(42,44,48,120); }"

        "QToolButton#TplSave {"
        "  background:rgba(60,60,60,150);"
        "  border:1px solid rgba(255,255,255,0.14);"
        "  border-radius:10px;"
        "}"
        "QToolButton#TplSave:hover { background:rgba(90,90,90,170); }"
        "QToolButton#TplSave:disabled { background:rgba(60,60,60,80); }"

        "QLabel#TplState {"
        "  border-radius:11px;"
        "  padding:2px 10px;"
        "  font-size:12px;"
        "  font-weight:600;"
        "}"

        "QLabel#TplState[state='empty'] {"
        "  color:rgba(255,255,255,0.45);"
        "  background:rgba(255,255,255,0.06);"
        "  border:1px solid rgba(255,255,255,0.08);"
        "}"
        "QLabel#TplState[state='off'] {"
        "  color:rgba(255,220,120,0.95);"
        "  background:rgba(255,200,80,0.12);"
        "  border:1px solid rgba(255,200,80,0.22);"
        "}"
        "QLabel#TplState[state='on'] {"
        "  color:rgba(120,255,180,0.95);"
        "  background:rgba(0,180,100,0.14);"
        "  border:1px solid rgba(0,180,100,0.26);"
        "}"
        "QToolButton#TplSaveAll {"
        "  padding:7px 12px;"
        "  background:rgba(60,60,60,150);"
        "  color:#f0f0f0;"
        "  border-radius:10px;"
        "  border:1px solid rgba(255,255,255,0.14);"
        "}"
        "QToolButton#TplSaveAll:hover { background:rgba(90,90,90,170); }"
    );

    refreshAll();
}

QString TemplateDialog::safeSeriesFolderName(QString series) const
{
    series = series.trimmed();
    if (series.isEmpty())
        series = "Unknown";

    // вычищаем то, что может сломать путь в Windows
    series.replace(QRegularExpression(R"([<>:"/\\|?*\x00-\x1F])"), "_");
    series = series.left(80); // чтобы не улететь в длинные пути
    return series;
}

void TemplateDialog::applyTexts()
{
    if (mHdrLeft)  mHdrLeft->setText(tr("Chambers"));
    if (mHdrRight) mHdrRight->setText(tr("Other"));

    // кнопки слева
    for (const auto& it : kLeftItems) {
        auto& r = mRows[idxOf(it.id)];
        if (r.toggle) r.toggle->setText(tr(it.key));
    }

    // кнопки справа
    for (const auto& it : kRightItems) {
        auto& r = mRows[idxOf(it.id)];
        if (r.toggle) r.toggle->setText(tr(it.key));
    }

    // тултип у save (одинаковый для всех)
    const QString tip = tr("Save current visible volume");
    for (auto& r : mRows) {
        if (r.save) r.save->setToolTip(tip);
    }

    if (mBtnClrScene)
        mBtnClrScene->setText(tr("Clear scene"));

    if (mBtnSaveAll)
        mBtnSaveAll->setText(tr("Save templates…"));

    // важно: статусы empty/on/off тоже должны обновиться
    refreshAll();
}


bool TemplateDialog::eventFilter(QObject* o, QEvent* e)
{
        return DialogShell::eventFilter(o, e);
}

void TemplateDialog::done(int r)
{
    if (m_onFinished) m_onFinished();
    QDialog::done(r);
}

void TemplateDialog::changeEvent(QEvent* e)
{
    DialogShell::changeEvent(e);

    if (e->type() == QEvent::LanguageChange)
    {
        retranslateUi();
        update();
    }
}

void TemplateDialog::onClearScene()
{
    emit requestClearScene();
}

void TemplateDialog::onSaveAllTemplates(bool hide, bool saveto, const QString& savedir)
{
    int count = 0;
    for (const auto& kv : mSlots)
        if (kv.second.hasData())
            ++count;

    if (count == 0) 
    {
        if(!hide)
            CustomMessageBox::information(this, tr("Templates"), tr("Nothing to save."), ServiceWindow);
        return;
    }

    if (!mDinfo) {
        if (!hide)
            CustomMessageBox::warning(this, tr("Templates"), tr("No DICOM info. Cannot determine output folder."), ServiceWindow);
        return;
    }

    
    QString dicomDir = mDinfo->DicomPath.trimmed();
    if (saveto)
        dicomDir = savedir.trimmed();
    
    if (dicomDir.isEmpty()) 
    {
        if (!hide)
            CustomMessageBox::warning(this, tr("Templates"), tr("DicomPath is empty."));
        return;
    }

    QDir base(dicomDir);
    if (!base.exists()) 
    {
        if (!hide)
            CustomMessageBox::warning(this, tr("Templates"),
            tr("DicomPath does not exist:\n%1").arg(dicomDir));
        return;
    }

    const QString series = safeSeriesFolderName(mDinfo->SeriesNumber);
    const QString tplFolderName = QString("Templates-Series-%1").arg(series);

    if (!base.exists(tplFolderName))
        if (!base.mkdir(tplFolderName)) 
        {
            if (!hide)
                CustomMessageBox::warning(this, tr("Templates"), tr("Failed to create folder:\n%1").arg(base.filePath(tplFolderName)));
            return;
        }

    if (!base.cd(tplFolderName)) 
    {
        if (!hide)
            CustomMessageBox::warning(this, tr("Templates"), tr("Failed to open folder:\n%1").arg(base.filePath(tplFolderName)));
        return;
    }

    const QString outDir = base.absolutePath();

    QStringList failed;

    for (const auto& kv : mSlots)
    {
        const TemplateId id = kv.first;
        const Slot& slot = kv.second;

        if (!slot.hasData())
            continue;

        const QString fileName = templateFileStem(id) + ".mini3dr";
        const QString filePath = base.filePath(fileName);

        if (!saveSlotTo3dr(filePath, slot))
            failed << fileName;
    }

    if (!failed.isEmpty()) 
    {
        if (!hide)
            CustomMessageBox::warning(this, tr("Templates"),
            tr("Saved to:\n%1\n\nSome templates were not saved:\n%2")
            .arg(outDir, failed.join("\n")));
    }
    else 
    {
        if (!hide)
            CustomMessageBox::information(this, tr("Templates"),
            tr("Saved %1 templates to:\n%2").arg(count).arg(outDir));
    }
}


bool TemplateDialog::saveSlotTo3dr(const QString& filePath, const Slot& s)
{
    if (!s.hasData())
        return false;

    vtkImageData* img = s.data.raw();
    if (!img)
        return false;

    if (!mDinfo)
        return false;

    QString err;
    const bool ok = Save3DR::writemini3dr(filePath, img, mDinfo, &err);
    if (!ok) {
        // если хочешь, можно показывать err, но лучше собирать в общий failed
        // qWarning() << "Save template failed:" << filePath << err;
    }
    return ok;
}


void TemplateDialog::retranslateUi()
{
    const QString title = tr("Templates");
    setWindowTitle(title);
    if (titleBar())
        titleBar()->setTitle(title);

    applyTexts();
}

bool TemplateDialog::isCaptured(TemplateId id)
{
    auto& s = mSlots[id];
    return s.hasData();
}

void TemplateDialog::setCaptured(TemplateId id, Volume img)
{
    auto& s = mSlots[id];
    s.data = img;
    s.visible = true;

    refreshAll();
}

void TemplateDialog::setVisibleInternal(TemplateId id, bool on)
{
    auto& s = mSlots[id];

    if (on && !s.hasData())
        return;

    s.visible = on;
    emit requestSetVisible(id, on);
    refreshAll();
}

const TemplateDialog::Slot* TemplateDialog::slot(TemplateId id) const
{
    auto it = mSlots.find(id);
    return it == mSlots.end() ? nullptr : &it->second;
}

TemplateDialog::Slot* TemplateDialog::slot(TemplateId id)
{
    auto it = mSlots.find(id);
    return it == mSlots.end() ? nullptr : &it->second;
}

// Папка: <DicomPath>/Templates-Series-<SeriesNumberSafe>
QString TemplateDialog::templatesFolderPath() const
{
    if (!mDinfo) return {};

    const QString dicomDir = mDinfo->DicomPath.trimmed();
    if (dicomDir.isEmpty()) return {};

    QDir base(dicomDir);
    if (!base.exists()) return {};

    const QString series = safeSeriesFolderName(mDinfo->SeriesNumber);
    const QString tplFolderName = QString("Templates-Series-%1").arg(series);

    // не создаём тут, только возвращаем путь
    return base.filePath(tplFolderName);
}

bool TemplateDialog::loadSlotFromMini3dr(TemplateId id,
    const QString& filePath,
    vtkImageData* seriesImage)
{
    if (!seriesImage)
        return false;

    auto img = vtkSmartPointer<vtkImageData>::New();

    img->CopyStructure(seriesImage);

    img->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

    QString err;
    if (!Save3DR::readmini3dr_into(filePath, img, &err)) 
        return false;

    img->Modified();

    Volume v;
    v.copy(img);

    auto& s = mSlots[id];
    s.data = std::move(v);
    s.visible = false;
    return true;
}


void TemplateDialog::loadAllTemplatesFromDisk(vtkImageData* seriesImage)
{
    if (!seriesImage)
        return;

    const QString folder = templatesFolderPath();
    if (folder.isEmpty())
        return;

    QDir dir(folder);
    if (!dir.exists())
        return;

    int loaded = 0;

    for (int i = 0; i < int(TemplateId::Count); ++i)
    {
        const auto id = TemplateId(i);
        const QString fileName = templateFileStem(id) + ".mini3dr";
        const QString filePath = dir.filePath(fileName);

        if (!QFileInfo::exists(filePath))
            continue;

        if (loadSlotFromMini3dr(id, filePath, seriesImage))
            ++loaded;
    }

    if (loaded > 0)
        refreshAll();
}

