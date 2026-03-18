#include "AppConfig.h"

#include <QFile>
#include <QSaveFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QDir>
#include <QCoreApplication>

static bool parseBoolLoose(const QString& s, bool def)
{
    const auto t = s.trimmed().toLower();
    if (t == "1" || t == "true" || t == "yes" || t == "on")  return true;
    if (t == "0" || t == "false" || t == "no" || t == "off") return false;
    return def;
}

QString AppConfig::defaultFilePath()
{
    return QCoreApplication::applicationDirPath() + "/settings.xml";
}

AppConfig AppConfig::loadCurrent()
{
    return loadOrCreateDefault(defaultFilePath());
}

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

    // ищем элементы внутри <ui>
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
            else if (inUi && name == u"showTooltips")
            {
                cfg.showTooltips = parseBoolLoose(xr.readElementText(), /*def=*/true);
            }
            else if (name == u"Electrodes")
            {
                cfg.electrodesEnabled = parseBoolLoose(xr.readElementText(), /*def=*/true);
            }
            else if (name == u"HDBASE")
            {
                cfg.hdBasePath = xr.attributes().value(u"path").toString().trimmed();
                xr.skipCurrentElement();
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
    xw.writeTextElement("showTooltips", showTooltips ? "true" : "false");
    xw.writeEndElement(); // ui

    xw.writeTextElement("Electrodes", electrodesEnabled ? "true" : "false");

    xw.writeStartElement("HDBASE");
    xw.writeAttribute("path", hdBasePath);
    xw.writeEndElement(); // HDBASE

    xw.writeEndElement(); // AstroTomoEditor
    xw.writeEndDocument();

    return f.commit();
}
