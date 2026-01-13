#include "SettingsDialog.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QComboBox>
#include <QSettings>
#include <Services/AppConfig.h>
#include <QCheckBox>
#include <Services/TooltipsFilter.h>

static constexpr const char* kLangKey = "ui/language"; // "ru" / "en"
static constexpr const char* kGradOpacityKey = "render/gradientOpacity";
static constexpr const char* kInterpKey = "render/volumeInterpolation"; // 0 nearest, 1 linear

SettingsDialog::SettingsDialog(QWidget* parent, bool mainstate)
    : DialogShell(parent, QObject::tr("Settings"), WindowType::Settings)
{
    setWindowFlag(Qt::Tool);
    setFixedSize(mSize);

    QWidget* content = contentWidget();
    content->setObjectName("SettingsDialogContent");

    auto* v = new QVBoxLayout(content);
    v->setContentsMargins(16, 12, 16, 16);
    v->setSpacing(10);

    mForm = new QFormLayout();
    mForm->setContentsMargins(5, 5, 5, 5);
    mForm->setHorizontalSpacing(20);
    mForm->setVerticalSpacing(20);
    mForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    mForm->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);

    v->addLayout(mForm);

    // --- UI: Language
    lblLang = new QLabel(QObject::tr("Language:"), content);
    lblLang->setProperty("role", "label");

    mLangCombo = new FixedDownComboBox(content);
    mLangCombo->addItem(tr("Russian"), "ru");
    mLangCombo->addItem(tr("English"), "en");


    mForm->addRow(lblLang, mLangCombo);

    connect(mLangCombo, &FixedDownComboBox::currentIndexChanged, this, [this](int idx)
        {
            const QString code = mLangCombo->itemData(idx).toString();
            saveLanguage(code);
            emit languageChanged(code);
        });

    lblTooltips = new QLabel(QObject::tr("Tooltips:"), content);
    lblTooltips->setProperty("role", "label");

    mShowTooltips = new QCheckBox(content);
    mShowTooltips->setText(tr("Show tooltips"));

    mForm->addRow(lblTooltips, mShowTooltips);

    connect(mShowTooltips, &QCheckBox::toggled, this, [this](bool on)
        {
            saveShowTooltips(on);
            TooltipsFilter::instance().setEnabled(on);
        });

    if (mainstate)
    {
        lblGradientOpacity = new QLabel(QObject::tr("Gradient opacity:"), content);
        lblGradientOpacity->setProperty("role", "label");

        mGradientOpacity = new QCheckBox(content);
        mGradientOpacity->setText(tr("Enable gradient opacity"));

        mGradientOpacity->setChecked(QSettings().value(kGradOpacityKey, true).toBool());

        mForm->addRow(lblGradientOpacity, mGradientOpacity);

        lblInterpolation = new QLabel(QObject::tr("Interpolation:"), content);
        lblInterpolation->setProperty("role", "label");

        mInterpolationCombo = new FixedDownComboBox(content);
        mInterpolationCombo->addItem(tr("Nearest"), 0);
        mInterpolationCombo->addItem(tr("Linear (smooth)"), 1);

        mForm->addRow(lblInterpolation, mInterpolationCombo);

        connect(mInterpolationCombo, &FixedDownComboBox::currentIndexChanged, this, [this](int idx)
            {
                const int mode = mInterpolationCombo->itemData(idx).toInt();
                saveVolumeInterpolation(mode);
                emit volumeInterpolationChanged(mode);
            });

        connect(mGradientOpacity, &QCheckBox::toggled, this, [this](bool on)
            {
                enableGradientOpacity(on);
            });

        QSize targetSize = mSize;
        targetSize.setHeight(targetSize.height() + 90);
        targetSize.setWidth(targetSize.width() + 90);
        setMinimumSize(targetSize);
        setMaximumSize(targetSize);
        resize(targetSize);
        updateGeometry();
    }
    
    // --- style
    content->setStyleSheet(
        "#SettingsDialogContent { background: transparent; }"
        "QLabel { color:#e6e6e6; }"
        "QLabel[role=\"label\"] { color:rgba(255,255,255,0.70); }"
        "QComboBox { color:#e6e6e6; background:#2b2d31; border:1px solid rgba(255,255,255,0.15); "
        "border-radius:6px; padding:4px 8px; }"
        "QComboBox::drop-down { border:0px; width:18px; }"
        "QComboBox QAbstractItemView { background:#2b2d31; color:#e6e6e6; selection-background-color:rgba(255,255,255,0.12); }"
        "QCheckBox {"
        "  color:#e6e6e6;"
        "}"
        "QCheckBox::indicator {"
        "  width:14px;"
        "  height:14px;"
        "}"
        "QCheckBox::indicator:unchecked {"
        "  border:1px solid rgba(255,255,255,0.5);"
        "  background:transparent;"
        "  border-radius:5px;"
        "}"
        "QCheckBox::indicator:checked {"
        "  background:rgba(255,255,255,0.6);"
        "  border:1px solid rgba(255,255,255,0.8);"
        "  border-radius:5px;"
        "}"
    );

    loadSettings();
    retranslateUi();
}


void SettingsDialog::loadSettings()
{
    const QString cfgPath = QCoreApplication::applicationDirPath() + "/settings.xml";
    const AppConfig cfg = AppConfig::loadOrCreateDefault(cfgPath);

    const int idx = mLangCombo->findData(cfg.language);
    if (idx >= 0)
        mLangCombo->setCurrentIndex(idx);

    if (mShowTooltips)
        mShowTooltips->setChecked(cfg.showTooltips);

    TooltipsFilter::instance().setEnabled(cfg.showTooltips);

    QSettings s;
    const bool gradOn = s.value(kGradOpacityKey, false).toBool();
    const int interp = s.value(kInterpKey, 0).toInt();

    if (mGradientOpacity)
        mGradientOpacity->setChecked(gradOn);

    if (mInterpolationCombo)
    {
        const int ii = mInterpolationCombo->findData(interp);
        if (ii >= 0)
            mInterpolationCombo->setCurrentIndex(ii);
    }
}


void SettingsDialog::saveLanguage(const QString& code)
{
    const QString cfgPath = QCoreApplication::applicationDirPath() + "/settings.xml";
    AppConfig cfg = AppConfig::loadOrCreateDefault(cfgPath);
    cfg.language = code;
    cfg.save(cfgPath);
}

void SettingsDialog::saveShowTooltips(bool on)
{
    const QString cfgPath = QCoreApplication::applicationDirPath() + "/settings.xml";
    AppConfig cfg = AppConfig::loadOrCreateDefault(cfgPath);
    cfg.showTooltips = on;
    cfg.save(cfgPath);
}

void SettingsDialog::enableGradientOpacity(bool on)
{
    saveGradientOpacity(on);
    emit gradientOpacityChanged(on);
}

void SettingsDialog::saveGradientOpacity(bool on)
{
    QSettings s;
    s.setValue(kGradOpacityKey, on);
}

void SettingsDialog::saveVolumeInterpolation(int mode)
{
    QSettings s;
    s.setValue(kInterpKey, mode);
}

void SettingsDialog::syncGradientOpacityUi(bool on)
{
    if (!mGradientOpacity) return;

    QSignalBlocker b(mGradientOpacity);     // чтобы не вызвать toggled и не уйти в круг
    mGradientOpacity->setChecked(on);
}

void SettingsDialog::retranslateUi()
{
    DialogShell::retranslateUi();

    const QString title = tr("Settings");
    setWindowTitle(title);
    if (titleBar())
        titleBar()->setTitle(title);

    if (lblLang)
        lblLang->setText(tr("Language:"));

    if (mLangCombo)
    {
        const QString currentCode = mLangCombo->currentData().toString();

        for (int i = 0; i < mLangCombo->count(); ++i)
        {
            const QString code = mLangCombo->itemData(i).toString();
            if (code == "ru")
                mLangCombo->setItemText(i, tr("Русский"));
            else if (code == "en")
                mLangCombo->setItemText(i, tr("English"));
        }

        const int idx = mLangCombo->findData(currentCode);
        if (idx >= 0)
            mLangCombo->setCurrentIndex(idx);
    }

    if (lblTooltips)
        lblTooltips->setText(tr("Tooltips:"));

    if (mShowTooltips)
        mShowTooltips->setText(tr("Show tooltips"));

    if (lblGradientOpacity)
        lblGradientOpacity->setText(tr("Gradient opacity:"));

    if (lblInterpolation)
        lblInterpolation->setText(tr("Interpolation:"));

    if (mGradientOpacity)
        mGradientOpacity->setText(tr("Enable gradient opacity"));

    if (mInterpolationCombo)
    {
        const int currentMode = mInterpolationCombo->currentData().toInt();

        for (int i = 0; i < mInterpolationCombo->count(); ++i)
        {
            const int mode = mInterpolationCombo->itemData(i).toInt();
            if (mode == 0)
                mInterpolationCombo->setItemText(i, tr("Nearest"));
            else if (mode == 1)
                mInterpolationCombo->setItemText(i, tr("Linear (smooth)"));
        }

        const int idx = mInterpolationCombo->findData(currentMode);
        if (idx >= 0)
            mInterpolationCombo->setCurrentIndex(idx);
    }
}
