#pragma once
#ifndef DIALOGSHELL_H
#define DIALOGSHELL_H

#include <QDialog>

class QWidget;
class TitleBar;

class DialogShell : public QDialog
{
public:
    explicit DialogShell(QWidget* parent = nullptr,
        const QString& title = QString());
    ~DialogShell() override;

    QWidget* contentWidget() const { return mContent; }
    TitleBar* titleBar() const { return mTitleBar; }

private:
    QWidget* mCard = nullptr;
    QWidget* mContent = nullptr;
    TitleBar* mTitleBar = nullptr;

    void buildUi(const QString& title);
    void applyStyle();
};

#endif // DIALOGSHELL_H
