#pragma once
#include "..\MainWindow\DialogShell.h"

class QFileDialog;

class ShellFileDialog final : public DialogShell
{
    Q_OBJECT
public:
    explicit ShellFileDialog(QWidget* parent,
        const QString& title,
        int typeofwindow,
        const QString& directory,
        const QString& filter);

    QFileDialog* fileDialog() const { return dlg_; }

private:
    QFileDialog* dlg_ = nullptr;
};
