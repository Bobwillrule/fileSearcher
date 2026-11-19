#include "organization/organization_store.h"
#include "storage/metadata_store.h"
#include <QSqlError>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFileInfo>
#include <QUuid>
#include <stdexcept>

namespace filesearcher {
QSqlQuery OrganizationStore::query(const QString& text,const QVariantList& values) const {
    QSqlQuery q(db_);
    if (!q.prepare(text)) throw std::runtime_error(q.lastError().text().toStdString());
    for (const auto& v:values) q.addBindValue(v);
    if (!q.exec()) throw std::runtime_error(q.lastError().text().toStdString());
    return q;
}
OrganizationStore::OrganizationStore(const QString& database) {
    connection_=QUuid::createUuid().toString(); db_=QSqlDatabase::addDatabase("QSQLITE",connection_);
    db_.setDatabaseName(database); db_.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000");
    try {
        if (!db_.open()) throw std::runtime_error("Cannot open collections and move history");
        query("PRAGMA journal_mode=WAL"); query("PRAGMA synchronous=FULL"); query("PRAGMA foreign_keys=ON");
        auto version=query("PRAGMA user_version"); version.next();
        if(version.value(0).toInt()>1) throw std::runtime_error("Organization data belongs to a newer application");
        query("CREATE TABLE IF NOT EXISTS categories(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,name_key TEXT UNIQUE NOT NULL,description TEXT NOT NULL,destination TEXT NOT NULL,examples TEXT NOT NULL DEFAULT '[]',revision INTEGER NOT NULL DEFAULT 1)");
        query("CREATE TABLE IF NOT EXISTS decisions(category INTEGER REFERENCES categories(id) ON DELETE CASCADE,path TEXT NOT NULL,size INTEGER,modified INTEGER,revision INTEGER,status TEXT NOT NULL,PRIMARY KEY(category,path))");
        query("CREATE TABLE IF NOT EXISTS members(category INTEGER REFERENCES categories(id) ON DELETE CASCADE,path_key TEXT NOT NULL,path TEXT NOT NULL,PRIMARY KEY(category,path_key))");
        query("CREATE TABLE IF NOT EXISTS moves(id TEXT PRIMARY KEY,body TEXT NOT NULL)"); query("PRAGMA user_version=1");
    } catch (...) { db_.close(); db_={}; QSqlDatabase::removeDatabase(connection_); throw; }
}
OrganizationStore::~OrganizationStore() { db_.close(); db_={}; QSqlDatabase::removeDatabase(connection_); }
QVariantList OrganizationStore::categories() const {
    auto q=query("SELECT id,name,description,destination,examples,revision,(SELECT count(*) FROM members WHERE category=categories.id) FROM categories ORDER BY name_key");
    QVariantList result;
    while(q.next()) result<<QVariantMap{{"id",q.value(0)},{"name",q.value(1)},{"description",q.value(2)},{"destination",q.value(3)},
        {"examples",QJsonDocument::fromJson(q.value(4).toByteArray()).array().toVariantList()},{"revision",q.value(5)},{"members",q.value(6)}};
    return result;
}
qint64 OrganizationStore::saveCategory(qint64 id,const QString& name,const QString& description,const QString& destination) {
    if(name.trimmed().isEmpty() || name.size()>80 || description.size()>2000) throw std::runtime_error("Use a category name of 1-80 characters and a description up to 2,000 characters");
    const auto folder=destination.trimmed().isEmpty()?QStringLiteral(""):normalizedPath(destination);
    if(!folder.isEmpty() && !QFileInfo(folder).isDir()) throw std::runtime_error("Choose an existing destination folder, or leave it empty for a virtual collection");
    if(id) {
        auto q=query("UPDATE categories SET name=?,name_key=?,description=?,destination=?,revision=revision+1 WHERE id=?",{name.trimmed(),name.trimmed().toCaseFolded(),description,folder,id});
        if(q.numRowsAffected()!=1) throw std::runtime_error("This category no longer exists"); return id;
    }
    return query("INSERT INTO categories(name,name_key,description,destination) VALUES(?,?,?,?)",{name.trimmed(),name.trimmed().toCaseFolded(),description,folder}).lastInsertId().toLongLong();
}
void OrganizationStore::removeCategory(qint64 id) { query("DELETE FROM categories WHERE id=?",{id}); }
void OrganizationStore::addExample(qint64 id,const QString& path) {
    auto q=query("SELECT examples FROM categories WHERE id=?",{id}); if(!q.next()) throw std::runtime_error("Category unavailable");
    auto examples=QJsonDocument::fromJson(q.value(0).toByteArray()).array();
    for(const auto& value:examples) if(pathKey(value.toString())==pathKey(path)) return;
    if(examples.size()>=12) throw std::runtime_error("A category can have up to 12 example files");
    examples.append(normalizedPath(path)); query("UPDATE categories SET examples=?,revision=revision+1 WHERE id=?",{QString::fromUtf8(QJsonDocument(examples).toJson(QJsonDocument::Compact)),id});
}
void OrganizationStore::decide(qint64 category,const QString& path,qint64 size,qint64 modified,const QString& decision) {
    if(decision!="accepted" && decision!="rejected") throw std::runtime_error("Invalid category decision");
    if(!db_.transaction()) throw std::runtime_error("Cannot save category decision");
    try {
        auto q=query("SELECT revision FROM categories WHERE id=?",{category}); if(!q.next()) throw std::runtime_error("Category unavailable");
        query("INSERT INTO decisions VALUES(?,?,?,?,?,?) ON CONFLICT(category,path) DO UPDATE SET size=excluded.size,modified=excluded.modified,revision=excluded.revision,status=excluded.status",{category,pathKey(path),size,modified,q.value(0),decision});
        if(decision=="accepted") query("INSERT INTO members VALUES(?,?,?) ON CONFLICT DO NOTHING",{category,pathKey(path),normalizedPath(path)});
        else query("DELETE FROM members WHERE category=? AND path_key=?",{category,pathKey(path)});
        if(!db_.commit()) throw std::runtime_error("Cannot save collection");
    } catch(...) { db_.rollback(); throw; }
}
QString OrganizationStore::decision(qint64 category,const QString& path,qint64 size,qint64 modified) const {
    auto q=query("SELECT status FROM decisions JOIN categories ON categories.id=category WHERE category=? AND path=? AND size=? AND modified=? AND decisions.revision=categories.revision",{category,pathKey(path),size,modified});
    return q.next()?q.value(0).toString():QString();
}
QVariantList OrganizationStore::members(qint64 category) const {
    auto q=query("SELECT path FROM members WHERE category=? ORDER BY path_key",{category}); QVariantList result;
    while(q.next()) { auto path=q.value(0).toString(); result<<QVariantMap{{"path",path},{"available",QFileInfo(path).isFile()}}; } return result;
}
QVariantList OrganizationStore::history() const {
    auto q=query("SELECT body FROM moves ORDER BY rowid DESC LIMIT 100"); QVariantList result;
    while(q.next()) result<<QJsonDocument::fromJson(q.value(0).toByteArray()).object().toVariantMap(); return result;
}
QJsonObject OrganizationStore::operation(const QString& id) const {
    auto q=query("SELECT body FROM moves WHERE id=?",{id}); if(!q.next()) throw std::runtime_error("Move record is unavailable");
    return QJsonDocument::fromJson(q.value(0).toByteArray()).object();
}
void OrganizationStore::putOperation(const QJsonObject& operation) {
    query("INSERT INTO moves VALUES(?,?) ON CONFLICT(id) DO UPDATE SET body=excluded.body",{operation["id"].toString(),QString::fromUtf8(QJsonDocument(operation).toJson(QJsonDocument::Compact))});
}
void OrganizationStore::complete(QJsonObject operation) {
    if(!db_.transaction()) throw std::runtime_error("Cannot finish move record");
    try {
        operation["state"]="completed"; operation["message"]="Move completed. Undo checks identity and contents before changing files."; putOperation(operation);
        if(!operation["undoOf"].toString().isEmpty()) { auto parent=this->operation(operation["undoOf"].toString()); parent["state"]="undone"; putOperation(parent); }
        query("INSERT OR IGNORE INTO members SELECT category,?,? FROM members WHERE path_key=?",{pathKey(operation["destination"].toString()),operation["destination"].toString(),pathKey(operation["source"].toString())});
        query("DELETE FROM members WHERE path_key=?",{pathKey(operation["source"].toString())});
        query("INSERT OR REPLACE INTO decisions SELECT category,?,size,modified,revision,status FROM decisions WHERE path=?",{pathKey(operation["destination"].toString()),pathKey(operation["source"].toString())});
        query("DELETE FROM decisions WHERE path=?",{pathKey(operation["source"].toString())});
        const auto savedCategories=categories();
        for(const auto& value:savedCategories) {
            const auto category=value.toMap(); QJsonArray examples; bool changed=false;
            for(const auto& example:category["examples"].toList()) {
                if(pathKey(example.toString())==pathKey(operation["source"].toString())) { examples.append(operation["destination"].toString()); changed=true; }
                else examples.append(example.toString());
            }
            if(changed) query("UPDATE categories SET examples=? WHERE id=?",{QString::fromUtf8(QJsonDocument(examples).toJson(QJsonDocument::Compact)),category["id"]});
        }
        if(!db_.commit()) throw std::runtime_error("Cannot finish move record");
    } catch(...) { db_.rollback(); throw; }
}
}
