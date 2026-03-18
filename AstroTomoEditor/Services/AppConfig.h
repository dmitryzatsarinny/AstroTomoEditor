#pragma once
#include <QString>

struct AppConfig
{
    QString language = "ru";   // ru/en
    bool showTooltips = true;
    bool electrodesEnabled = true;
    QString hdBasePath;

    static QString defaultFilePath();
    static AppConfig loadCurrent();
    static AppConfig loadOrCreateDefault(const QString& filePath);
    bool save(const QString& filePath) const;
};
