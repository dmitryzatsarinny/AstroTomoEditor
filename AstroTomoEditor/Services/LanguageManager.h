#pragma once
#include <QObject>
#include <QTranslator>
#include <memory>

class LanguageManager : public QObject
{
    Q_OBJECT
public:
    static LanguageManager& instance();

    QString language() const { return mLang; }
    bool setLanguage(const QString& code); // "ru"/"en"

signals:
    void languageChanged();

private:
    LanguageManager() = default;

    QString mLang = "ru";
    std::unique_ptr<QTranslator> mAppTr;
    std::unique_ptr<QTranslator> mQtTr;
};
