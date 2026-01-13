#include "LanguageManager.h"

#include <QApplication>
#include <QTranslator>
#include <QDebug>
#include <QLibraryInfo>

LanguageManager& LanguageManager::instance()
{
    static LanguageManager inst;
    return inst;
}

bool LanguageManager::setLanguage(const QString& code)
{
    const QString lang = (code == "ru" || code == "en") ? code : "ru";

    const bool alreadyInstalled = (mAppTr && mQtTr);
    if (lang == mLang && alreadyInstalled)
        return true;

    if (mAppTr) qApp->removeTranslator(mAppTr.get());
    if (mQtTr)  qApp->removeTranslator(mQtTr.get());

    mAppTr = std::make_unique<QTranslator>();
    mQtTr = std::make_unique<QTranslator>();

    // --- Qt base (для QFileSystemModel, стандартных диалогов и т.п.)
    bool okQt = mQtTr->load(":/i18n/qtbase_" + lang + ".qm");
    if (!okQt) {
        // fallback: dev-среда
        okQt = mQtTr->load("qtbase_" + lang, QLibraryInfo::path(QLibraryInfo::TranslationsPath));
    }

    // --- App translations
    const bool okApp = mAppTr->load(":/i18n/AstroDicomEditor_" + lang + ".qm"); // или AstroTomoEditor_
    // ^ проверь имя!

    qDebug() << "LanguageManager::setLanguage"
        << "lang=" << lang
        << "okQt=" << okQt
        << "okApp=" << okApp;

    if (okQt)  qApp->installTranslator(mQtTr.get());
    if (okApp) qApp->installTranslator(mAppTr.get());

    mLang = lang;
    emit languageChanged();
    return okApp;
}

