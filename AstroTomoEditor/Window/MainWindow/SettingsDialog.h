#pragma once

#include "DialogShell.h"
#include <Services/Pool.h>
#include <QEvent>
#include "TitleBar.h"

class QLabel;
class QFormLayout;
class QComboBox;

class SettingsDialog : public DialogShell
{
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr, bool mainstate = true);
    void retranslateUi();
    void changeEvent(QEvent* e) override
    {
        QDialog::changeEvent(e);
        if (e->type() == QEvent::LanguageChange)
            retranslateUi();
    }
    void syncGradientOpacityUi(bool on);

signals:
    void languageChanged(const QString& code);
    void gradientOpacityChanged(bool on);
    void volumeInterpolationChanged(int mode); // 0 nearest, 1 linear

private:
    void saveLanguage(const QString& code);
    void saveShowTooltips(bool on);
    void enableGradientOpacity(bool on);
    void loadSettings();
    void saveGradientOpacity(bool on);
    void saveVolumeInterpolation(int mode);

private:

    QFormLayout* mForm = nullptr;

    QLabel* lblLang = nullptr;
    FixedDownComboBox* mLangCombo = nullptr;

    QLabel* lblTooltips = nullptr;
    QCheckBox* mShowTooltips = nullptr;

    QLabel* lblGradientOpacity = nullptr;
    QCheckBox* mGradientOpacity = nullptr;

    QLabel* lblInterpolation = nullptr;
    FixedDownComboBox* mInterpolationCombo = nullptr;

    const QSize mSize{ 420, 140 };
};