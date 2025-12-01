#include "DialogShell.h"
#include "..\MainWindow\TitleBar.h"

#include <QVBoxLayout>
#include <QWidget>
#include <QIcon>

DialogShell::DialogShell(QWidget* parent,
    const QString& title)
    : QDialog(parent)
{
    // Без рамки, с нашим собственным заголовком
    setObjectName("DialogShell");
    setWindowFlags(Qt::Dialog
        | Qt::FramelessWindowHint
        | Qt::CustomizeWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);

    // Базовый размер, потом конкретный диалог может сделать resize()
    resize(520, 360);

    // Иконка приложения (можешь убрать, если нужно)
    setWindowIcon(QIcon(":/icons/Resources/dicom_heart.ico"));

    buildUi(title);
    applyStyle();
}

DialogShell::~DialogShell() = default;

void DialogShell::buildUi(const QString& title)
{
    // Внешний отступ от прозрачного фона до карточки
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(0);

    // Скруглённая карточка
    mCard = new QWidget(this);
    mCard->setObjectName("CentralCard");
    mCard->setAttribute(Qt::WA_StyledBackground, true);
    outer->addWidget(mCard);

    auto* cardLayout = new QVBoxLayout(mCard);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    // TitleBar сверху
    mTitleBar = new TitleBar(this, 1, title);
    mTitleBar->setObjectName("TitleBar");
    cardLayout->addWidget(mTitleBar, 0);

    // Пустой контейнер для содержимого диалога
    mContent = new QWidget(mCard);
    mContent->setObjectName("DialogContent");
    // Лэйаут специально не задаём — ты сам поставишь нужный во внешнем коде
    cardLayout->addWidget(mContent, 1);
}

void DialogShell::applyStyle()
{
    // Минимальный общий стиль: тёмная карточка с радиусом, контент прозрачный
    mCard->setStyleSheet(
        "#CentralCard {"
        "  background:#1f2023;"
        "  border:1px solid rgba(255,255,255,0.14);"
        "  border-radius:10px;"
        "}"
        "#DialogContent {"
        "  background:transparent;"
        "}"
    );
}