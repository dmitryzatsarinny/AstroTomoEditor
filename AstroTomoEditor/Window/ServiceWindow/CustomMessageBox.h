#pragma once
#ifndef CUSTOMMESSAGEBOX_H
#define CUSTOMMESSAGEBOX_H


#include "..\MainWindow\DialogShell.h"

class QLabel;
class QPushButton;

class CustomMessageBox final : public DialogShell
{
    Q_OBJECT
public:
    enum class Icon { Info, Warning, Error, Question };
    enum class Buttons { Ok, OkCancel, YesNo, YesNoCancel };

    explicit CustomMessageBox(QWidget* parent,
        Icon icon,
        const QString& title,
        const QString& text,
        Buttons buttons = Buttons::Ok,
        int windowType = WindowType::ServiceWindow);

    static void information(QWidget* parent, const QString& title, const QString& text,
        int windowType = WindowType::ServiceWindow);

    static void warning(QWidget* parent, const QString& title, const QString& text,
        int windowType = WindowType::ServiceWindow);

    static void error(QWidget* parent, const QString& title, const QString& text,
        int windowType = WindowType::ServiceWindow);

    static bool questionYesNo(QWidget* parent, const QString& title, const QString& text,
        int windowType = WindowType::ServiceWindow);

    static bool questionOkCancel(QWidget* parent, const QString& title, const QString& text,
        int windowType = WindowType::ServiceWindow);

    static void critical(QWidget* parent, const QString& title, const QString& text,
        int windowType = WindowType::ServiceWindow);

private:
    QLabel* mIcon = nullptr;
    QLabel* mText = nullptr;

    QPushButton* mB1 = nullptr;
    QPushButton* mB2 = nullptr;
    QPushButton* mB3 = nullptr;

    void buildUi(Icon icon, const QString& text, Buttons buttons);
    void applyLocalStyle(Icon icon);

    void setupButtons(Buttons buttons);
    void wireButtons(Buttons buttons);

    static QString defaultTitle(Icon icon);
};

#endif // CUSTOMMESSAGEBOX_H
