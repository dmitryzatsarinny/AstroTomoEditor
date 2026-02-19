#include "CustomMessageBox.h"

#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include "..\MainWindow\TitleBar.h"

static QString iconGlyph(CustomMessageBox::Icon icon)
{
    // Чтобы не зависеть от ресурсов: используем символы.
    // Можешь заменить на svg/png в QLabel::setPixmap.
    switch (icon) {
    case CustomMessageBox::Icon::Info:     return "i";
    case CustomMessageBox::Icon::Warning:  return "!";
    case CustomMessageBox::Icon::Error:    return "×";
    case CustomMessageBox::Icon::Question: return "?";
    }
    return "?";
}

QString CustomMessageBox::defaultTitle(Icon icon)
{
    switch (icon) {
    case Icon::Info:     return tr("Information");
    case Icon::Warning:  return tr("Warning");
    case Icon::Error:    return tr("Error");
    case Icon::Question: return tr("Question");
    }
    return tr("Message");
}


CustomMessageBox::CustomMessageBox(QWidget* parent,
    Icon icon,
    const QString& title,
    const QString& text,
    Buttons buttons,
    int windowType)
    : DialogShell(parent,
        title.isEmpty() ? defaultTitle(icon) : title,
        windowType)
{
    setObjectName("CustomMessageBox");
    setSizeGripEnabled(false);
    buildUi(icon, text, buttons);
    applyLocalStyle(icon);
}

static QString wrapPathNice(QString s)
{
    const QChar zwsp(0x200B);

    s.replace("/", QString("/").append(zwsp));
    s.replace("\\", QString("\\").append(zwsp));

    return s;
}

void CustomMessageBox::buildUi(Icon icon, const QString& text, Buttons buttons)
{
    auto* root = new QVBoxLayout(contentWidget());
    root->setContentsMargins(16, 12, 16, 12);
    root->setSpacing(8);

    // BODY
    auto* body = new QWidget(contentWidget());
    body->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

    auto* bodyLay = new QHBoxLayout(body);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    bodyLay->setSpacing(12);

    mIcon = new QLabel(body);
    mIcon->setObjectName("MsgIcon");
    mIcon->setFixedSize(34, 34);
    mIcon->setAlignment(Qt::AlignCenter);
    mIcon->setText(iconGlyph(icon));

    mText = new QLabel(body);
    mText->setObjectName("MsgText");
    mText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mText->setWordWrap(true);
    mText->setText(wrapPathNice(text));
    mText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    mText->setFixedWidth(260);

    bodyLay->addWidget(mIcon, 0, Qt::AlignCenter);
    bodyLay->addWidget(mText, 1, Qt::AlignTop);

    root->addWidget(body, 0);

    // FOOTER
    auto* footer = new QWidget(contentWidget());
    footer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* footerLay = new QHBoxLayout(footer);
    footerLay->setContentsMargins(0, 0, 0, 0);
    footerLay->setSpacing(10);

    mB1 = new QPushButton(footer);
    mB2 = new QPushButton(footer);
    mB3 = new QPushButton(footer);

    mB1->setObjectName("Btn1");
    mB2->setObjectName("Btn2");
    mB3->setObjectName("Btn3");

    setupButtons(buttons);

    footerLay->addStretch(1);
    footerLay->addWidget(mB3); // Cancel
    footerLay->addWidget(mB2); // No
    footerLay->addWidget(mB1); // OK / Yes

    root->addWidget(footer, 0);

    wireButtons(buttons);

    // 1) сначала пусть диалог примет нормальную ширину
    adjustSize();
    contentWidget()->layout()->activate();
    setFixedSize(size());
}


void CustomMessageBox::setupButtons(Buttons buttons)
{
    mB1->hide();
    mB2->hide();
    mB3->hide();

    auto setup = [&](QPushButton* b, const QString& text, bool def = false)
        {
            b->setText(text);
            b->setDefault(def);
            b->show();
            b->setFixedWidth(60);
            b->setFixedHeight(25);
            b->setCursor(Qt::PointingHandCursor);
        };

    switch (buttons)
    {
    case Buttons::Ok:
        setup(mB1, tr("OK"), true);
        break;

    case Buttons::OkCancel:
        setup(mB3, tr("Cancel"));
        setup(mB1, tr("OK"), true);
        break;

    case Buttons::YesNo:
        setup(mB2, tr("No"));
        setup(mB1, tr("Yes"), true);
        break;

    case Buttons::YesNoCancel:
        setup(mB3, tr("Cancel"));
        setup(mB2, tr("No"));
        setup(mB1, tr("Yes"), true);
        break;
    }
}

void CustomMessageBox::wireButtons(Buttons buttons)
{
    // Результаты:
    // accept() -> "позитивная" кнопка
    // reject() -> "негативная/отмена"

    auto acceptBtn = mB1; // по нашей раскладке позитивная всегда первая

    connect(acceptBtn, &QPushButton::clicked, this, &QDialog::accept);

    // остальные -> reject
    if (mB2->isVisible())
        connect(mB2, &QPushButton::clicked, this, &QDialog::reject);
    if (mB3->isVisible())
        connect(mB3, &QPushButton::clicked, this, &QDialog::reject);

    // Escape закрывает как Cancel/No
    // (у QDialog это дефолтно, если есть reject path)
}

void CustomMessageBox::applyLocalStyle(Icon icon)
{
    // Цвет иконки под тип (аккуратно, без кислотности)
    QString iconBg;
    QString iconBorder;
    QString iconFg;

    switch (icon) {
    case Icon::Info:
        iconBg = "rgba(0,120,215,0.18)";
        iconBorder = "rgba(0,120,215,0.55)";
        iconFg = "rgba(220,235,255,0.95)";
        break;
    case Icon::Warning:
        iconBg = "rgba(255,170,0,0.16)";
        iconBorder = "rgba(255,170,0,0.55)";
        iconFg = "rgba(255,235,200,0.95)";
        break;
    case Icon::Error:
        iconBg = "rgba(220,60,60,0.16)";
        iconBorder = "rgba(220,60,60,0.55)";
        iconFg = "rgba(255,220,220,0.95)";
        break;
    case Icon::Question:
        iconBg = "rgba(160,160,160,0.14)";
        iconBorder = "rgba(180,180,180,0.35)";
        iconFg = "rgba(240,240,240,0.92)";
        break;
    }

    contentWidget()->setStyleSheet(QString(R"(
        QLabel#MsgText {
            background: transparent;
            color: rgba(255,255,255,0.88);
            font-size: 12px;
            padding: 0px;
            margin: 0px;
        }
        QLabel#MsgIcon {
            border-radius: 8px;
            background: %1;
            border: 1px solid %2;
            color: %3;
            font-size: 20px;
            font-weight: 700;
        }
        QPushButton#Btn1, QPushButton#Btn2, QPushButton#Btn3 {
            min-width: 60px;
            padding: 6px 14px;
            border-radius: 6px;
            border: 1px solid rgba(255,255,255,0.18);
            background: rgba(255,255,255,0.06);
            color: rgba(255,255,255,0.90);
        }
        QPushButton#Btn1:hover, QPushButton#Btn2:hover, QPushButton#Btn3:hover {
            background: rgba(255,255,255,0.10);
        }
    )").arg(iconBg, iconBorder, iconFg));
}

// --- статические хелперы ---

void CustomMessageBox::information(QWidget* parent, const QString& title, const QString& text, int windowType)
{
    CustomMessageBox dlg(parent, Icon::Info, title, text, Buttons::Ok, windowType);
    dlg.exec();
}

void CustomMessageBox::warning(QWidget* parent, const QString& title, const QString& text, int windowType)
{
    CustomMessageBox dlg(parent, Icon::Warning, title, text, Buttons::Ok, windowType);
    dlg.exec();
}

void CustomMessageBox::error(QWidget* parent, const QString& title, const QString& text, int windowType)
{
    CustomMessageBox dlg(parent, Icon::Error, title, text, Buttons::Ok, windowType);
    dlg.exec();
}

bool CustomMessageBox::questionYesNo(QWidget* parent, const QString& title, const QString& text, int windowType)
{
    CustomMessageBox dlg(parent, Icon::Question, title, text, Buttons::YesNo, windowType);
    return dlg.exec() == QDialog::Accepted;
}

bool CustomMessageBox::questionOkCancel(QWidget* parent, const QString& title, const QString& text, int windowType)
{
    CustomMessageBox dlg(parent, Icon::Question, title, text, Buttons::OkCancel, windowType);
    return dlg.exec() == QDialog::Accepted;
}

void CustomMessageBox::critical(QWidget* parent, const QString& title, const QString& text, int windowType)
{
    CustomMessageBox dlg(parent, Icon::Error, title, text, Buttons::Ok, windowType);
    dlg.exec();
}
