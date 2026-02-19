#pragma once
#ifndef DIALOGSHELL_H
#define DIALOGSHELL_H

#include <QDialog>

class QWidget;
class TitleBar;

enum WindowType
{
    Main = 0,
    Explorer = 1,
    Settings = 2,
    Patient = 3,
    Histogram = 4,
    TranferFunction = 5,
    Template = 6,
    ServiceWindow = 7
};

class DialogShell : public QDialog
{
public:
    explicit DialogShell(QWidget* parent = nullptr,
        const QString& title = QString(), const int typeofwindow = WindowType::Main);
    ~DialogShell() override;

    QWidget* contentWidget() const { return mContent; }
    TitleBar* titleBar() const { return mTitleBar; }
    void retranslateUi();
private:
    QWidget* mCard = nullptr;
    QWidget* mContent = nullptr;
    TitleBar* mTitleBar = nullptr;

    void buildUi(const QString& title, const int typeofwindow = WindowType::Main);
    void applyStyle();
};

#endif // DIALOGSHELL_H
