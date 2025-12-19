#include "LanguageManager.h"
#include <QApplication>
#include <qfile.h>

LanguageManager& LanguageManager::instance()
{
    static LanguageManager inst;
    return inst;
}

bool LanguageManager::setLanguage(const QString& code)
{
    const bool alreadyInstalled = (mAppTr && mQtTr);
    if (code == mLang && alreadyInstalled)
        return true;

    if (mAppTr) qApp->removeTranslator(mAppTr.get());
    if (mQtTr)  qApp->removeTranslator(mQtTr.get());

    mAppTr = std::make_unique<QTranslator>();
    mQtTr = std::make_unique<QTranslator>();

    const bool okApp = mAppTr->load(":/i18n/AstroDicomEditor_" + code + ".qm");
    const bool okQt = mQtTr->load(":/i18n/qtbase_" + code + ".qm");

    if (okQt)  qApp->installTranslator(mQtTr.get());
    if (okApp) qApp->installTranslator(mAppTr.get());

    mLang = code;
    emit languageChanged();
    return okApp;
}
