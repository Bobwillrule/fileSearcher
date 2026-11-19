#include "cleanup/cleanup_service.h"
#include "cleanup/file_checks.h"
#include "cleanup/recycle_file.h"
#include "storage/metadata_store.h"
#include <QSqlError>
#include <QJsonDocument>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QUuid>
#include <QMap>
#include <stdexcept>

namespace filesearcher {
namespace {
struct Reader {
    QString name=QUuid::createUuid().toString(); QSqlDatabase db=QSqlDatabase::addDatabase("QSQLITE",name);
    explicit Reader(const QString& path) { db.setDatabaseName(path); db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=2000"); if(!db.open()) { db={}; QSqlDatabase::removeDatabase(name); throw std::runtime_error("Index unavailable"); } }
    ~Reader(){db.close();db={};QSqlDatabase::removeDatabase(name);}
};
QString reason(const QString& type) {
    if(type=="exact") return "Same size, SHA-256 and byte-for-byte contents. Keep the named copy; review the other file's location before recycling.";
    if(type=="build") return "Recognized generated extension/name inside a build folder. Opt-in suggestion only: verify that you can rebuild it.";
    return "Installer filename/type and more than 180 days since modification. Age is not proof it is disposable; verify you no longer need this installer.";
}
void validateRule(const QJsonObject& candidate) {
    if(candidate["kind"]=="exact") return;
    const auto path=candidate["path"].toString(); QFileInfo f(path);
    if(cleanup::rule(path,f.lastModified().toMSecsSinceEpoch(),candidate["kind"]=="installer",candidate["kind"]=="build")!=candidate["kind"].toString()) throw std::runtime_error("The selected cleanup rule no longer matches");
}
}
CleanupService::CleanupService(OrganizationStore& store,QStringList folders,const std::atomic_bool& cancelled):store_(store),protected_(std::move(folders)),cancelled_(cancelled) {
    store_.query("CREATE TABLE IF NOT EXISTS cleanup_operations(id TEXT PRIMARY KEY,body TEXT NOT NULL)");
}
void CleanupService::put(const QJsonObject& op) { store_.query("INSERT INTO cleanup_operations VALUES(?,?) ON CONFLICT(id) DO UPDATE SET body=excluded.body",{op["id"].toString(),QString::fromUtf8(QJsonDocument(op).toJson(QJsonDocument::Compact))}); }
QJsonObject CleanupService::operation(const QString& id) const { auto q=store_.query("SELECT body FROM cleanup_operations WHERE id=?",{id}); if(!q.next()) throw std::runtime_error("Cleanup review unavailable"); return QJsonDocument::fromJson(q.value(0).toByteArray()).object(); }
QVariantList CleanupService::history() const { auto q=store_.query("SELECT body FROM cleanup_operations ORDER BY rowid DESC LIMIT 100"); QVariantList r; while(q.next()) r<<QJsonDocument::fromJson(q.value(0).toByteArray()).object().toVariantMap(); return r; }
CleanupScan CleanupService::scan(const QString& metadata,qint64 root,bool installers,bool builds) {
    if(root<=0) throw std::runtime_error("Choose one indexed folder as the cleanup scope");
    Reader reader(metadata); QSqlQuery q(reader.db);
    q.prepare("SELECT files.id,path,size,modified FROM files JOIN root_files ON file_id=files.id WHERE root_id=? ORDER BY size,path_key LIMIT 50001"); q.addBindValue(root);
    if(!q.exec()) throw std::runtime_error(q.lastError().text().toStdString());
    QMap<qint64,QList<QVariantMap>> sizes; CleanupScan result; int count=0;
    while(q.next()) { cleanup::stop(cancelled_); if(++count>50000) { result.limited=true; break; } sizes[q.value(2).toLongLong()]<<QVariantMap{{"fileId",q.value(0)},{"path",q.value(1)},{"size",q.value(2)},{"modified",q.value(3)}}; } q.finish();
    for(const auto& rows:sizes) {
        QMap<QString,QList<QJsonObject>> hashes;
        for(const auto& row:rows) {
            cleanup::stop(cancelled_);
            if(result.candidates.size()>=1000) { result.limited=true; return result; }
            const auto path=row["path"].toString(),rule=cleanup::rule(path,row["modified"].toLongLong(),installers,builds);
            if(rows.size()<2 && rule.isEmpty()) continue;
            ++result.inspected;
            try {
                cleanup::guard(path,protected_,rule=="build"); auto pinned=cleanup::parents(path); auto file=cleanup::open(path);
                QFileInfo current(path); if(current.size()!=row["size"].toLongLong() || current.lastModified().toMSecsSinceEpoch()!=row["modified"].toLongLong()) throw std::runtime_error("Stale index entry");
                const auto snap=cleanup::snapshot(file->value,cancelled_);
                QJsonObject candidate{{"token",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"path",path},{"snapshot",snap},{"keep",""},{"keepSnapshot",QJsonObject{}},{"fileId",row["fileId"].toLongLong()},
                    {"allocated",snap["allocated"]},{"size",snap["size"]},{"modified",current.lastModified().toUTC().toString(Qt::ISODate)},{"kind",rule}};
                bool duplicate=false;
                // Generated files are reviewable only through their explicit build rule.
                if(rule!="build") {
                    auto& matches=hashes[snap["sha256"].toString()];
                    for(const auto& keep:matches) {
                        auto kp=cleanup::parents(keep["path"].toString()); auto retained=cleanup::open(keep["path"].toString());
                        cleanup::expect(retained->value,keep["snapshot"].toObject(),cancelled_);
                        if(cleanup::sameFile(snap,keep["snapshot"].toObject()) || !cleanup::equal(file->value,retained->value,cancelled_)) continue;
                        candidate["kind"]="exact"; candidate["keep"]=keep["path"]; candidate["keepSnapshot"]=keep["snapshot"]; duplicate=true; break;
                    }
                    if(!duplicate) matches<<candidate;
                }
                if(duplicate || !rule.isEmpty()) { candidate["reason"]=reason(candidate["kind"].toString()); result.candidates<<candidate.toVariantMap(); }
            } catch(const std::exception&) { cleanup::stop(cancelled_); ++result.skipped; }
        }
    }
    return result;
}
QJsonObject CleanupService::prepare(QJsonObject candidate,bool reverse) {
    if(candidate["kind"]!="exact" && candidate["kind"]!="build" && candidate["kind"]!="installer") throw std::runtime_error("Similar documents and possible versions cannot be recycled from suggestions");
    if(reverse && candidate["kind"]=="exact") { const QJsonValue path=candidate["path"],snap=candidate["snapshot"]; candidate["path"]=candidate["keep"]; candidate["snapshot"]=candidate["keepSnapshot"]; candidate["keep"]=path; candidate["keepSnapshot"]=snap; candidate["allocated"]=candidate["snapshot"].toObject()["allocated"]; }
    const auto path=candidate["path"].toString(); cleanup::guard(path,protected_,candidate["kind"]=="build"); validateRule(candidate);
    auto pins=cleanup::parents(path); auto file=cleanup::open(path); cleanup::expect(file->value,candidate["snapshot"].toObject(),cancelled_);
    if(candidate["kind"]=="exact") {
        const auto keep=candidate["keep"].toString(); cleanup::guard(keep,protected_); auto kp=cleanup::parents(keep); auto retained=cleanup::open(keep);
        cleanup::expect(retained->value,candidate["keepSnapshot"].toObject(),cancelled_);
        if(cleanup::sameFile(candidate["snapshot"].toObject(),candidate["keepSnapshot"].toObject()) || !cleanup::equal(file->value,retained->value,cancelled_)) throw std::runtime_error("The two files are no longer separate identical copies");
    }
    candidate["id"]=QUuid::createUuid().toString(QUuid::WithoutBraces); candidate["state"]="planned"; candidate["createdAt"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODate); candidate["message"]="Awaiting separate approval. Files unchanged.";
    put(candidate); return candidate;
}
void CleanupService::cancel(const QString& id) { auto op=operation(id); if(op["state"]!="planned") throw std::runtime_error("This operation has already started"); op["state"]="cancelled"; op["message"]="Review cancelled; files unchanged."; put(op); }
QJsonObject CleanupService::execute(const QString& id) {
    auto op=operation(id); if(op["state"]!="planned") throw std::runtime_error("Only a newly approved review can run; interrupted operations are never retried automatically");
    try {
        cleanup::stop(cancelled_); const auto path=op["path"].toString(); cleanup::guard(path,protected_,op["kind"]=="build"); validateRule(op);
        auto pins=cleanup::parents(path); auto file=cleanup::open(path,true);
        std::vector<cleanup::Held> keepPins; cleanup::Held retained;
        if(op["kind"]=="exact") { const auto keep=op["keep"].toString(); cleanup::guard(keep,protected_); keepPins=cleanup::parents(keep); retained=cleanup::open(keep); }
        auto validate=[&] {
            cleanup::stop(cancelled_); cleanup::expect(file->value,op["snapshot"].toObject(),cancelled_);
            // Reopen by name in the Shell callback to detect renames/replacements.
            auto named=cleanup::open(path,true); cleanup::expect(named->value,op["snapshot"].toObject(),cancelled_);
            if(retained) { cleanup::expect(retained->value,op["keepSnapshot"].toObject(),cancelled_); if(cleanup::sameFile(op["snapshot"].toObject(),op["keepSnapshot"].toObject()) || !cleanup::equal(file->value,retained->value,cancelled_)) throw std::runtime_error("Retained copy is no longer identical"); }
        };
        validate(); if(checkpoint) checkpoint("validated");
        op["state"]="recycling"; op["message"]="Approved intent saved. Windows recycling requested."; put(op); if(checkpoint) checkpoint("recycling");
        op["recycledPath"]=recycleFile(path,validate,cancelled_); if(checkpoint) checkpoint("recycled");
        op["state"]="recycled"; op["message"]="Windows confirmed recycling. Restore through Windows Recycle Bin; no app-managed undo. Disk space is not freed until the bin is emptied."; put(op);
    } catch(const std::exception& e) {
        op["state"]=op["state"]=="recycling"?"needs_review":"failed"; op["message"]=QString::fromUtf8(e.what()); put(op); throw;
    }
    return op;
}
void CleanupService::recover() {
    auto q=store_.query("SELECT body FROM cleanup_operations"); QList<QJsonObject> pending; while(q.next()) { auto op=QJsonDocument::fromJson(q.value(0).toByteArray()).object(); if(op["state"]=="recycling") pending<<op; } q.finish();
    for(auto op:pending) { op["state"]="needs_review"; op["message"]="Interrupted recycling: inspect the original path and Windows Recycle Bin. No automatic retry or deletion was performed."; put(op); }
}
}
