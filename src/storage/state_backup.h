#pragma once
#include <QString>
#include <QJsonObject>
#include <functional>
namespace filesearcher {
inline constexpr auto releaseVersion="0.2.0";
// Caller must hold the application's index.lock and have no active state writers.
QString prepareStateUpgrade(const QString& directory,const std::function<void(const QString&)>& checkpoint={});
void finishStateUpgrade(const QString& directory);
QJsonObject verifyStateBackup(const QString& backupDirectory);
void restoreStateBackup(const QString& backupDirectory,const QString& newDirectory);
}
