#pragma once

#include "DialogShell.h"
#include <Services/Pool.h>

class QLabel;
class QFormLayout;
class QComboBox;

class SettingsDialog : public DialogShell
{
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    void retranslateUi();
signals:
    void languageChanged(const QString& code);

private:
    void buildUi();
    void loadSettings();
    void saveLanguage(const QString& code);
    

private:

    QFormLayout* mForm = nullptr;
    QComboBox* mLangCombo = nullptr;
    QLabel* lblLang = nullptr;

    const QSize mSize{ 420, 160 };
};