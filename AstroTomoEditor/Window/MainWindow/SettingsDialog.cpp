#include "SettingsDialog.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QComboBox>
#include <QSettings>

static constexpr const char* kLangKey = "ui/language"; // "ru" / "en"

SettingsDialog::SettingsDialog(QWidget* parent)
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
    mForm->setContentsMargins(0, 0, 0, 0);
    mForm->setHorizontalSpacing(8);
    mForm->setVerticalSpacing(4);
    mForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    mForm->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);

    v->addLayout(mForm);

    // --- UI: Language
    lblLang = new QLabel(QObject::tr("Language"), content);
    lblLang->setProperty("role", "label");

    mLangCombo = new FixedDownComboBox(content);
    mLangCombo->addItem("Русский", "ru");
    mLangCombo->addItem("English", "en");

    mForm->addRow(lblLang, mLangCombo);

    connect(mLangCombo, &FixedDownComboBox::currentIndexChanged, this, [this](int idx)
        {
            const QString code = mLangCombo->itemData(idx).toString();
            saveLanguage(code);
            emit languageChanged(code);
        });

    // --- style
    content->setStyleSheet(
        "#SettingsDialogContent { background: transparent; }"
        "QLabel { color:#e6e6e6; }"
        "QLabel[role=\"label\"] { color:rgba(255,255,255,0.70); }"
        "QComboBox { color:#e6e6e6; background:#2b2d31; border:1px solid rgba(255,255,255,0.15); "
        "border-radius:6px; padding:4px 8px; }"
        "QComboBox::drop-down { border:0px; width:18px; }"
        "QComboBox QAbstractItemView { background:#2b2d31; color:#e6e6e6; selection-background-color:rgba(255,255,255,0.12); }"
    );

    loadSettings();
}

void SettingsDialog::loadSettings()
{
    QSettings s;
    const QString lang = s.value(kLangKey, "ru").toString();
    const int idx = mLangCombo->findData(lang);
    if (idx >= 0)
        mLangCombo->setCurrentIndex(idx);
}

void SettingsDialog::saveLanguage(const QString& code)
{
    QSettings s;
    s.setValue(kLangKey, code);
    s.sync();
}

void SettingsDialog::retranslateUi()
{
    retranslateUi();

    if (lblLang)
        lblLang->setText(tr("Language"));
}
