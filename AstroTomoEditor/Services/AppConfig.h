#pragma once
#include <QString>

struct AppConfig
{
    QString language = "ru";   // ru/en

    static AppConfig loadOrCreateDefault(const QString& filePath);
    bool save(const QString& filePath) const;
};
