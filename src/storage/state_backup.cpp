#include "storage/state_backup.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QVersionNumber>
#include <QUuid>
#include <QSet>
#include <stdexcept>

namespace filesearcher {
namespace {
const QStringList stateFiles{"metadata.sqlite","organization.sqlite","work.sqlite","preferences.ini","release-state.json"};
QJsonObject readJson(const QString& path) {
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read state manifest");
    QJsonParseError error{}; auto doc=QJsonDocument::fromJson(f.readAll(),&error);
    if(error.error!=QJsonParseError::NoError || !doc.isObject()) throw std::runtime_error("State manifest is invalid; do not discard the backup"); return doc.object();
}
void writeJson(const QString& path,const QJsonObject& value) {
    QSaveFile f(path); if(!f.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot save upgrade state; existing state was retained");
    const auto bytes=QJsonDocument(value).toJson();
    if(f.write(bytes)!=bytes.size() || !f.commit()) throw std::runtime_error("Cannot commit upgrade state; check free disk space");
}
QString hash(const QString& path) {
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read state backup file"); QCryptographicHash h(QCryptographicHash::Sha256);
    if(!h.addData(&f)) throw std::runtime_error("Cannot hash state backup file"); return QString::fromLatin1(h.result().toHex());
}
struct Database {
    QString name=QUuid::createUuid().toString(); QSqlDatabase db=QSqlDatabase::addDatabase("QSQLITE",name);
    explicit Database(const QString& path) { db.setDatabaseName(path); db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"); if(!db.open()) {db={};QSqlDatabase::removeDatabase(name);throw std::runtime_error("Cannot read existing state; upgrade stopped before migration");} }
    ~Database(){db.close();db={};QSqlDatabase::removeDatabase(name);}
    QSqlQuery query(const QString& sql,const QVariantList& values={}) { QSqlQuery q(db); if(!q.prepare(sql)) throw std::runtime_error("Cannot prepare state backup"); for(const auto& v:values) q.addBindValue(v); if(!q.exec()) throw std::runtime_error(("State backup failed: "+q.lastError().text()).toStdString()); return q; }
};
void checkDatabase(const QString& path) { Database db(path); auto q=db.query("PRAGMA quick_check"); if(!q.next() || q.value(0).toString()!="ok" || q.next()) throw std::runtime_error("Backup database failed its integrity check"); }
}
QJsonObject verifyStateBackup(const QString& directory) {
    const auto manifest=readJson(directory+"/manifest.json");
    if(manifest["format"].toInt()!=1 || !manifest["complete"].toBool() || manifest["files"].toArray().isEmpty()) throw std::runtime_error("Backup is incomplete or unsupported; live state was not changed");
    QSet<QString> seen;
    for(const auto& item:manifest["files"].toArray()) {
        const auto row=item.toObject(); const auto name=row["name"].toString(),path=directory+'/'+name;
        if(!stateFiles.contains(name) || seen.contains(name) || QFileInfo(path).isSymLink()) throw std::runtime_error("Invalid backup member"); seen.insert(name);
        if(hash(path)!=row["sha256"].toString()) throw std::runtime_error("Backup checksum mismatch; live state was not changed");
        if(name.endsWith(".sqlite")) checkDatabase(path);
    }
    return manifest;
}
QString prepareStateUpgrade(const QString& directory,const std::function<void(const QString&)>& checkpoint) {
    // Reject future schemas before opening any writable database or copying state.
    for(const auto& pair:{std::pair{QString("metadata.sqlite"),3},std::pair{QString("organization.sqlite"),1}}) {
        auto path=directory+'/'+pair.first; if(!QFileInfo::exists(path)) continue; Database db(path); auto v=db.query("PRAGMA user_version");
        if(!v.next() || v.value(0).toInt()>pair.second) throw std::runtime_error("State belongs to a newer application. Use that version; no downgrade was attempted.");
    }
    const auto marker=directory+"/release-state.json",pending=directory+"/upgrade.pending.json";
    if(QFileInfo::exists(marker)) {
        const auto current=readJson(marker);
        if(current["format"].toInt()!=1 || QVersionNumber::fromString(current["release"].toString())>QVersionNumber::fromString(releaseVersion)) throw std::runtime_error("State belongs to a newer release; downgrade was refused");
        if(current["release"]==releaseVersion && !QFileInfo::exists(pending)) return {};
    }
    if(QFileInfo::exists(pending)) {
        const auto p=readJson(pending); const auto leaf=p["backup"].toString();
        if(p["release"]!=releaseVersion || leaf.isEmpty() || leaf.contains('/') || leaf.contains('\\') || leaf.contains(':') || leaf=="..") throw std::runtime_error("Unrecognized interrupted upgrade. Preserve the backup and review recovery instructions.");
        const auto path=directory+"/backups/"+leaf; verifyStateBackup(path); return path;
    }
    QStringList existing; for(const auto& name:stateFiles) if(QFileInfo::exists(directory+'/'+name)) existing<<name;
    if(existing.isEmpty()) return {};
    const auto leaf=QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss")+'-'+QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto backup=directory+"/backups/"+leaf;
    if(!QDir().mkpath(backup)) throw std::runtime_error("Cannot create pre-upgrade backup; upgrade stopped");
    QJsonArray files;
    for(const auto& name:existing) {
        const auto source=directory+'/'+name,target=backup+'/'+name;
        if(name.endsWith(".sqlite")) { Database db(source); db.query("VACUUM INTO ?",{target}); }
        else if(!QFile::copy(source,target)) throw std::runtime_error("Cannot copy preferences/release state; upgrade stopped");
        files.append(QJsonObject{{"name",name},{"sha256",hash(target)}});
        if(checkpoint) checkpoint("copied");
    }
    writeJson(backup+"/manifest.json",{{"format",1},{"complete",true},{"targetRelease",releaseVersion},{"createdAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},{"files",files}});
    verifyStateBackup(backup); if(checkpoint) checkpoint("backup_verified");
    writeJson(pending,{{"release",releaseVersion},{"backup",leaf}}); if(checkpoint) checkpoint("pending_saved"); return backup;
}
void finishStateUpgrade(const QString& directory) {
    writeJson(directory+"/release-state.json",{{"format",1},{"release",releaseVersion}});
    const auto pending=directory+"/upgrade.pending.json";
    if(QFileInfo::exists(pending) && !QFile::remove(pending)) throw std::runtime_error("Upgrade succeeded but its pending marker could not be cleared; preserve the backup");
}
void restoreStateBackup(const QString& backup,const QString& destination) {
    const auto manifest=verifyStateBackup(backup);
    if(QFileInfo::exists(destination)) throw std::runtime_error("Restore destination must be a new folder. Existing state is never overwritten.");
    if(!QDir().mkpath(destination)) throw std::runtime_error("Cannot create restore destination");
    for(const auto& value:manifest["files"].toArray()) {
        const auto row=value.toObject(); const auto name=row["name"].toString();
        if(!QFile::copy(backup+'/'+name,destination+'/'+name) || hash(destination+'/'+name)!=row["sha256"].toString()) throw std::runtime_error("Restore incomplete. Original state and backup are unchanged; choose a new destination to retry.");
    }
    // Derived HNSW files are deliberately omitted and rebuilt from saved vectors.
    writeJson(destination+"/restored-from.json",{{"backup",QDir(backup).absolutePath()},{"restoredAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
}
}
