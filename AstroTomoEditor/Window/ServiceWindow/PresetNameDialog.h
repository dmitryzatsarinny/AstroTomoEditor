#pragma once
#ifndef PRESETNAMEDIALOG_H
#define PRESETNAMEDIALOG_H

#include "..\MainWindow\DialogShell.h"

class QLineEdit;
class QPushButton;

class PresetNameDialog final : public DialogShell
{
    Q_OBJECT
public:
    explicit PresetNameDialog(QWidget* parent,
        const QString& title,
        const QString& labelText,
        const QString& defaultValue = QString(),
        int windowType = WindowType::TranferFunction);

    QString textValue() const;

private:
    QLineEdit* mEdit = nullptr;
    QPushButton* mOk = nullptr;
    QPushButton* mCancel = nullptr;

    void buildForm(const QString& labelText, const QString& defaultValue);
    void applyLocalStyle();
};

#endif // PRESETNAMEDIALOG_H
