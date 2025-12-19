#include "AppConfig.h"

#include <QFile>
#include <QSaveFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QDir>

AppConfig AppConfig::loadOrCreateDefault(const QString& filePath)
{
    AppConfig cfg;

    QFile f(filePath);
    if (!f.exists())
    {
        cfg.save(filePath);
        return cfg;
    }

    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return cfg;

    QXmlStreamReader xr(&f);

    // ищем <language> внутри <ui>
    bool inUi = false;

    while (!xr.atEnd())
    {
        xr.readNext();

        if (xr.isStartElement())
        {
            const auto name = xr.name();

            if (name == u"ui")
            {
                inUi = true;
            }
            else if (inUi && name == u"language")
            {
                cfg.language = xr.readElementText().trimmed().toLower();
            }
        }
        else if (xr.isEndElement())
        {
            if (xr.name() == u"ui")
                inUi = false;
        }
    }

    if (cfg.language != "ru" && cfg.language != "en")
        cfg.language = "ru";

    return cfg;
}

bool AppConfig::save(const QString& filePath) const
{
    QSaveFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QXmlStreamWriter xw(&f);
    xw.setAutoFormatting(true);
    xw.writeStartDocument("1.0");
    xw.writeStartElement("AstroTomoEditor");
    xw.writeAttribute("version", "1");

    xw.writeStartElement("ui");
    xw.writeTextElement("language", language);
    xw.writeEndElement(); // ui

    xw.writeEndElement(); // AstroTomoEditor
    xw.writeEndDocument();

    return f.commit();
}
