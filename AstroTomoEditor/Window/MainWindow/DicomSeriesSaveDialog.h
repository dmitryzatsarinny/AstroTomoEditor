#pragma once
#include "DialogShell.h"
#include <Services/Pool.h> 
#include <Window/MainWindow/TitleBar.h>

class QLabel;
class QFormLayout;

class DicomSeriesSaveDialog : public DialogShell
{
    Q_OBJECT
public:
    explicit DicomSeriesSaveDialog(QWidget* parent = nullptr);

    void retranslateUi();

private:

    QFormLayout* mForm = nullptr;

    const QSize mSize{ 420, 170 };

};