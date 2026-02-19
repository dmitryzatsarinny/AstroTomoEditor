#include "PresetNameDialog.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>

PresetNameDialog::PresetNameDialog(QWidget* parent,
    const QString& title,
    const QString& labelText,
    const QString& defaultValue,
    int windowType)
    : DialogShell(parent, title, windowType)
{
    setObjectName("PresetNameDialog");

    // компактный размер под твой скрин
    resize(360, 160);

    buildForm(labelText, defaultValue);
    applyLocalStyle();
}

void PresetNameDialog::buildForm(const QString& labelText, const QString& defaultValue)
{
    auto* root = new QVBoxLayout(contentWidget());
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    auto* lbl = new QLabel(labelText, contentWidget());
    lbl->setObjectName("LblName");
    root->addWidget(lbl);

    mEdit = new QLineEdit(contentWidget());
    mEdit->setObjectName("EditName");
    mEdit->setText(defaultValue);
    mEdit->selectAll();
    root->addWidget(mEdit);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);

    mOk = new QPushButton(tr("OK"), contentWidget());
    mOk->setObjectName("BtnOk");

    mCancel = new QPushButton(tr("Cancel"), contentWidget());
    mCancel->setObjectName("BtnCancel");

    btnRow->addWidget(mOk);
    btnRow->addWidget(mCancel);
    root->addLayout(btnRow);

    // Логика
    mOk->setDefault(true);
    mOk->setEnabled(!mEdit->text().trimmed().isEmpty());

    connect(mEdit, &QLineEdit::textChanged, this, [this](const QString& s) {
        mOk->setEnabled(!s.trimmed().isEmpty());
        });

    connect(mEdit, &QLineEdit::returnPressed, this, [this] {
        if (mOk->isEnabled())
            accept();
        });

    connect(mOk, &QPushButton::clicked, this, &QDialog::accept);
    connect(mCancel, &QPushButton::clicked, this, &QDialog::reject);

    // фокус сразу в поле
    mEdit->setFocus();
}

QString PresetNameDialog::textValue() const
{
    return mEdit ? mEdit->text().trimmed() : QString();
}

void PresetNameDialog::applyLocalStyle()
{
    // Локальный стиль, чтобы гарантировать читаемость даже если глобальная тема “плывёт”
    // Если у тебя уже есть общий QSS, можно выкинуть или ослабить.
    contentWidget()->setStyleSheet(R"(
        QLabel#LblName { color: rgba(255,255,255,0.88); }

        QLineEdit#EditName {
            background: #141518;
            color: rgba(255,255,255,0.92);
            border: 1px solid rgba(255,255,255,0.18);
            border-radius: 6px;
            padding: 6px 8px;
            selection-background-color: #0078d7;
            selection-color: white;
        }
        QLineEdit#EditName:focus {
            border: 1px solid rgba(0,120,215,0.95);
        }

        QPushButton#BtnOk, QPushButton#BtnCancel {
            min-width: 92px;
            padding: 6px 14px;
            border-radius: 6px;
            border: 1px solid rgba(255,255,255,0.18);
            background: rgba(255,255,255,0.06);
            color: rgba(255,255,255,0.90);
        }
        QPushButton#BtnOk:hover, QPushButton#BtnCancel:hover {
            background: rgba(255,255,255,0.10);
        }
        QPushButton#BtnOk:disabled {
            color: rgba(255,255,255,0.35);
            border-color: rgba(255,255,255,0.10);
            background: rgba(255,255,255,0.03);
        }
    )");
}
