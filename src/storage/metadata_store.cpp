#include "storage/metadata_store.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <stdexcept>
#include <algorithm>
#include <QRegularExpression>

namespace filesearcher {
namespace {
void check(bool ok, const QSqlQuery& query) {
    if (!ok) throw std::runtime_error(("Metadata database: " + query.lastError().text()).toStdString());
}
QSqlQuery execute(QSqlDatabase db, const QString& sql, const QVariantList& values = {}) {
    QSqlQuery query(db);
    check(query.prepare(sql), query);
    for (const auto& value : values) query.addBindValue(value);
    check(query.exec(), query);
    return query;
}
QString encoded(const QStringList& exclusions) { return QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(exclusions)).toJson(QJsonDocument::Compact)); }
QString escapedLike(QString value) { return value.replace("!", "!!").replace("%", "!%").replace("_", "!_"); }
}
QString normalizedPath(const QString& path) { return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath())); }
QString pathKey(const QString& path) { return normalizedPath(path).toCaseFolded(); }
bool withinPath(const QString& path, const QString& parent) {
    const auto child = pathKey(path), root = pathKey(parent);
    return child == root || child.startsWith(root.endsWith('/') ? root : root + '/');
}
MetadataStore::MetadataStore(const QString& database, bool initialize) {
    connection_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    db_ = QSqlDatabase::addDatabase("QSQLITE", connection_);
    db_.setDatabaseName(database);
    db_.setConnectOptions("QSQLITE_BUSY_TIMEOUT=2000");
    if (!db_.open()) {
        const auto message = db_.lastError().text();
        db_ = QSqlDatabase(); QSqlDatabase::removeDatabase(connection_);
        throw std::runtime_error(message.toStdString());
    }
    try {
        execute(db_, "PRAGMA foreign_keys=ON");
        if (!initialize) return;
        execute(db_, "PRAGMA journal_mode=WAL");
        auto version = execute(db_, "PRAGMA user_version"); version.next();
        if (version.value(0).toInt() > 3) throw std::runtime_error("Index was created by a newer application version");
        version.finish();
        if (!db_.transaction()) throw std::runtime_error("Cannot initialize metadata database");
        execute(db_, "CREATE TABLE IF NOT EXISTS roots(id INTEGER PRIMARY KEY,path TEXT NOT NULL,path_key TEXT NOT NULL UNIQUE,exclusions TEXT NOT NULL,status TEXT NOT NULL DEFAULT 'Not scanned')");
        // A removed row's ID must never identify a different file in an older UI result.
        execute(db_, "CREATE TABLE IF NOT EXISTS files(id INTEGER PRIMARY KEY AUTOINCREMENT,path TEXT NOT NULL,path_key TEXT NOT NULL UNIQUE,name TEXT NOT NULL,name_key TEXT NOT NULL,extension TEXT NOT NULL,size INTEGER NOT NULL,modified INTEGER NOT NULL)");
        execute(db_, "CREATE TABLE IF NOT EXISTS root_files(root_id INTEGER NOT NULL REFERENCES roots(id) ON DELETE CASCADE,file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,generation TEXT NOT NULL,PRIMARY KEY(root_id,file_id))");
        execute(db_, "CREATE INDEX IF NOT EXISTS file_names ON files(name_key)");
        execute(db_, "CREATE INDEX IF NOT EXISTS file_types ON files(extension)");
        execute(db_, "CREATE INDEX IF NOT EXISTS memberships ON root_files(file_id)");
        execute(db_, "CREATE TABLE IF NOT EXISTS documents(file_id INTEGER PRIMARY KEY REFERENCES files(id) ON DELETE CASCADE,size INTEGER NOT NULL,modified INTEGER NOT NULL,status TEXT NOT NULL,attempts INTEGER NOT NULL)");
        execute(db_, "CREATE TABLE IF NOT EXISTS passages(id INTEGER PRIMARY KEY AUTOINCREMENT,file_id INTEGER NOT NULL REFERENCES documents(file_id) ON DELETE CASCADE,ordinal INTEGER NOT NULL,text TEXT NOT NULL,location TEXT NOT NULL,tokens INTEGER NOT NULL,UNIQUE(file_id,ordinal))");
        execute(db_, "CREATE VIRTUAL TABLE IF NOT EXISTS passages_fts USING fts5(text,content='passages',content_rowid='id',tokenize='unicode61 remove_diacritics 2')");
        execute(db_, "CREATE TRIGGER IF NOT EXISTS passages_insert AFTER INSERT ON passages BEGIN INSERT INTO passages_fts(rowid,text) VALUES(new.id,new.text); END");
        execute(db_, "CREATE TRIGGER IF NOT EXISTS passages_delete AFTER DELETE ON passages BEGIN INSERT INTO passages_fts(passages_fts,rowid,text) VALUES('delete',old.id,old.text); END");
        execute(db_, "CREATE TRIGGER IF NOT EXISTS content_invalidate AFTER UPDATE ON files WHEN old.path<>new.path OR old.size<>new.size OR old.modified<>new.modified BEGIN DELETE FROM documents WHERE file_id=new.id; END");
        execute(db_, "CREATE TABLE IF NOT EXISTS embedding_models(fingerprint TEXT PRIMARY KEY,dimensions INTEGER NOT NULL DEFAULT 384 CHECK(dimensions=384),generation INTEGER NOT NULL DEFAULT 0)");
        execute(db_, "CREATE TABLE IF NOT EXISTS embedding_chunks(id INTEGER PRIMARY KEY AUTOINCREMENT,passage_id INTEGER NOT NULL REFERENCES passages(id) ON DELETE CASCADE,model TEXT NOT NULL REFERENCES embedding_models(fingerprint),ordinal INTEGER NOT NULL,start_offset INTEGER NOT NULL,length INTEGER NOT NULL,text_hash BLOB NOT NULL,tokens INTEGER NOT NULL,vector BLOB CHECK(vector IS NULL OR length(vector)=1536),UNIQUE(passage_id,model,ordinal))");
        execute(db_, "CREATE INDEX IF NOT EXISTS embedding_pending ON embedding_chunks(model,id) WHERE vector IS NULL");
        execute(db_, "CREATE INDEX IF NOT EXISTS embedding_ready ON embedding_chunks(model,passage_id,id) WHERE vector IS NOT NULL");
        execute(db_, "CREATE TRIGGER IF NOT EXISTS embedding_update AFTER UPDATE OF vector ON embedding_chunks BEGIN UPDATE embedding_models SET generation=generation+1 WHERE fingerprint=new.model; END");
        execute(db_, "CREATE TRIGGER IF NOT EXISTS embedding_delete AFTER DELETE ON embedding_chunks WHEN old.vector IS NOT NULL BEGIN UPDATE embedding_models SET generation=generation+1 WHERE fingerprint=old.model; END");
        execute(db_, "PRAGMA user_version=3");
        if (!db_.commit()) throw std::runtime_error("Cannot commit index initialization");
    } catch (...) { db_.rollback(); db_.close(); db_ = QSqlDatabase(); QSqlDatabase::removeDatabase(connection_); throw; }
}
MetadataStore::~MetadataStore() { db_.close(); db_ = QSqlDatabase(); QSqlDatabase::removeDatabase(connection_); }
qint64 MetadataStore::addRoot(const QString& path, const QStringList& exclusions) {
    auto key = pathKey(path);
    execute(db_, "INSERT INTO roots(path,path_key,exclusions) VALUES(?,?,?) ON CONFLICT(path_key) DO NOTHING", {normalizedPath(path),key,encoded(exclusions)});
    auto result = execute(db_, "SELECT id FROM roots WHERE path_key=?", {key}); result.next();
    return result.value(0).toLongLong();
}
QList<IndexedRoot> MetadataStore::roots() const {
    QList<IndexedRoot> result;
    auto query = execute(db_, "SELECT id,path,exclusions,status FROM roots ORDER BY path_key");
    while (query.next()) {
        QStringList exclusions;
        for (const auto& value : QJsonDocument::fromJson(query.value(2).toByteArray()).array()) exclusions << value.toString();
        result.push_back({query.value(0).toLongLong(),query.value(1).toString(),exclusions,query.value(3).toString()});
    }
    return result;
}
void MetadataStore::updateExclusions(qint64 id, const QStringList& exclusions) { execute(db_, "UPDATE roots SET exclusions=?,status='Settings changed; rescan needed' WHERE id=?", {encoded(exclusions),id}); }
void MetadataStore::removeRoot(qint64 id) {
    if (!db_.transaction()) throw std::runtime_error("Cannot remove indexed folder");
    try {
        execute(db_, "DELETE FROM roots WHERE id=?", {id});
        execute(db_, "DELETE FROM files WHERE NOT EXISTS(SELECT 1 FROM root_files WHERE file_id=files.id)");
        if (!db_.commit()) throw std::runtime_error("Cannot commit folder removal");
    } catch (...) { db_.rollback(); throw; }
}
void MetadataStore::rootStatus(qint64 id, const QString& status) { execute(db_, "UPDATE roots SET status=? WHERE id=?", {status,id}); }
void MetadataStore::beginScan() { if (!db_.transaction()) throw std::runtime_error("Cannot start scan transaction"); }
void MetadataStore::record(qint64 root, const QString& generation, const FileMetadata& file) {
    const auto key = pathKey(file.path);
    const auto extension = file.extension.isNull() ? QStringLiteral("") : file.extension.toCaseFolded();
    execute(db_, "INSERT INTO files(path,path_key,name,name_key,extension,size,modified) VALUES(?,?,?,?,?,?,?) ON CONFLICT(path_key) DO UPDATE SET path=excluded.path,name=excluded.name,name_key=excluded.name_key,extension=excluded.extension,size=excluded.size,modified=excluded.modified WHERE files.path<>excluded.path OR files.name<>excluded.name OR files.extension<>excluded.extension OR files.size<>excluded.size OR files.modified<>excluded.modified",
        {file.path,key,file.name,file.name.toCaseFolded(),extension,file.size,file.modified});
    execute(db_, "INSERT INTO root_files(root_id,file_id,generation) SELECT ?,id,? FROM files WHERE path_key=? ON CONFLICT(root_id,file_id) DO UPDATE SET generation=excluded.generation", {root,generation,key});
}
void MetadataStore::finishScan(qint64 root, const QString& generation, bool reconcile) {
    if (reconcile) {
        execute(db_, "DELETE FROM root_files WHERE root_id=? AND generation<>?", {root,generation});
        execute(db_, "DELETE FROM files WHERE NOT EXISTS(SELECT 1 FROM root_files WHERE file_id=files.id)");
    }
    if (!db_.commit()) throw std::runtime_error("Cannot publish scan");
}
void MetadataStore::rollback() { db_.rollback(); }
SearchPage MetadataStore::search(const SearchOptions& options) const {
    QString where = " WHERE 1=1";
    QVariantList values;
    const auto text = QDir::fromNativeSeparators(options.query.trimmed()).toCaseFolded();
    const auto like = escapedLike(text);
    if (options.query.size() > 1024) throw std::runtime_error("Please shorten the search to 1,024 characters or fewer.");
    QString cte = "WITH best AS (SELECT NULL AS file_id,NULL AS passage,NULL AS score,NULL AS hit,NULL AS location WHERE 0) ";
    const bool contents = options.mode > 0 && !text.isEmpty();
    if (contents) {
        auto quote = [](QString value) { return '"' + value.replace("\"","\"\"") + '"'; };
        QString expression;
        if (options.mode == 4) expression = options.query.trimmed();
        else if (options.mode == 3) expression = quote(options.query.trimmed());
        else {
            const auto terms = options.query.trimmed().split(QRegularExpression("\\s+"),Qt::SkipEmptyParts);
            if (terms.size() > 64) throw std::runtime_error("Use at most 64 search terms.");
            QStringList escaped; for (const auto& term : terms) escaped << quote(term);
            expression = escaped.join(" AND ");
        }
        values << expression;
        cte = "WITH matches AS MATERIALIZED (SELECT rowid AS passage,bm25(passages_fts) AS score,snippet(passages_fts,0,'[',']',' … ',32) AS hit FROM passages_fts WHERE passages_fts MATCH ?), ranked AS (SELECT passages.file_id,matches.passage,matches.score,matches.hit,passages.location,row_number() OVER(PARTITION BY passages.file_id ORDER BY matches.score,matches.passage) AS position FROM matches JOIN passages ON passages.id=matches.passage), best AS (SELECT * FROM ranked WHERE position=1) ";
    }
    if (!text.isEmpty()) {
        if (options.mode < 2) {
            where += " AND (name_key LIKE ? ESCAPE '!' OR path_key LIKE ? ESCAPE '!'";
            if (contents) where += " OR best.file_id IS NOT NULL";
            where += ')'; values << '%' + like + '%' << '%' + like + '%';
        } else where += " AND best.file_id IS NOT NULL";
    }
    auto extension = options.extension.trimmed().toCaseFolded();
    if (extension.startsWith('.')) extension.remove(0, 1);
    if (!extension.isEmpty()) { where += " AND extension=?"; values << extension; }
    if (options.root > 0) { where += " AND EXISTS(SELECT 1 FROM root_files WHERE file_id=files.id AND root_id=?)"; values << options.root; }
    if (options.minimumSize >= 0) { where += " AND files.size>=?"; values << options.minimumSize; }
    if (options.maximumSize >= 0) { where += " AND files.size<=?"; values << options.maximumSize; }
    if (options.modifiedAfter >= 0) { where += " AND files.modified>=?"; values << options.modifiedAfter; }
    SearchPage result;
    const QString from = " FROM files LEFT JOIN best ON best.file_id=files.id LEFT JOIN documents ON documents.file_id=files.id";
    auto count = execute(db_, cte + "SELECT count(*)" + from + where, values); count.next(); result.total = count.value(0).toLongLong();
    QString order = " ORDER BY ";
    if (options.mode < 2) {
        order += "CASE WHEN name_key=? THEN 0 WHEN name_key LIKE ? ESCAPE '!' THEN 1 WHEN name_key LIKE ? ESCAPE '!' OR path_key LIKE ? ESCAPE '!' THEN 2 ELSE 3 END,";
        values << text << like + '%' << '%' + like + '%' << '%' + like + '%';
    }
    order += "COALESCE(best.score,0),name_key,path_key LIMIT ?";
    values << std::clamp(options.limit, 1, 2000);
    auto query = execute(db_, cte + "SELECT files.id,path,name,extension,files.size,files.modified,COALESCE(documents.status,'pending'),COALESCE(best.passage,0),COALESCE(best.hit,''),COALESCE(best.location,'')" + from + where + order, values);
    while (query.next()) result.rows << QVariantMap{{"fileId",query.value(0)},{"path",query.value(1)},{"name",query.value(2)},{"extension",query.value(3)},{"size",query.value(4)},{"modified",query.value(5)},{"contentStatus",query.value(6)},{"passageId",query.value(7)},{"snippet",query.value(8)},{"location",query.value(9)}};
    return result;
}
QString MetadataStore::filePath(qint64 id) const { auto query = execute(db_, "SELECT path FROM files WHERE id=?", {id}); return query.next() ? query.value(0).toString() : QString(); }
QList<ContentFile> MetadataStore::pendingContent(qint64 root, qint64 after) const {
    auto query = execute(db_,"SELECT files.id,path,files.size,files.modified,COALESCE(documents.attempts,0) FROM files LEFT JOIN documents ON documents.file_id=files.id WHERE files.id>? AND EXISTS(SELECT 1 FROM root_files WHERE root_id=? AND file_id=files.id) AND (documents.file_id IS NULL OR (documents.attempts<3 AND documents.status IN ('read_failed','worker_failed','timeout','start_failed','changed_during_read','missing_file'))) ORDER BY files.id LIMIT 100",{after,root});
    QList<ContentFile> result;
    while (query.next()) result << ContentFile{query.value(0).toLongLong(),query.value(1).toString(),query.value(2).toLongLong(),query.value(3).toLongLong(),query.value(4).toInt()};
    return result;
}
bool MetadataStore::replaceContent(const ContentFile& file, const QString& status, const QList<Passage>& passages) {
    if (!db_.transaction()) throw std::runtime_error("Cannot publish extracted text");
    try {
        auto current = execute(db_,"SELECT id FROM files WHERE id=? AND path=? AND size=? AND modified=?",{file.id,file.path,file.size,file.modified});
        if (!current.next()) { current.finish(); db_.rollback(); return false; }
        current.finish();
        execute(db_,"DELETE FROM documents WHERE file_id=?",{file.id});
        execute(db_,"INSERT INTO documents(file_id,size,modified,status,attempts) VALUES(?,?,?,?,?)",{file.id,file.size,file.modified,status,file.attempts+1});
        int ordinal = 0;
        for (const auto& passage : passages) execute(db_,"INSERT INTO passages(file_id,ordinal,text,location,tokens) VALUES(?,?,?,?,?)",{file.id,ordinal++,passage.text,passage.location,passage.tokens});
        if (!db_.commit()) throw std::runtime_error("Cannot commit extracted text");
        return true;
    } catch (...) { db_.rollback(); throw; }
}
void MetadataStore::resetContent() { execute(db_,"DELETE FROM documents"); }
void MetadataStore::clearSavedIndex(bool embeddingsOnly) {
    if (!db_.transaction()) throw std::runtime_error("Cannot clear the local index");
    try {
        if (embeddingsOnly) execute(db_,"DELETE FROM embedding_chunks");
        else { execute(db_,"DELETE FROM roots"); execute(db_,"DELETE FROM files"); }
        if (!db_.commit()) throw std::runtime_error("Cannot save index changes");
    } catch (...) { db_.rollback(); throw; }
    // Preserve monotonic IDs and model generations so old results cannot be reused.
    execute(db_,"PRAGMA wal_checkpoint(TRUNCATE)");
    execute(db_,"VACUUM");
}
QVariantMap MetadataStore::statistics() const {
    auto q=execute(db_,"SELECT (SELECT count(*) FROM files),(SELECT count(*) FROM passages),(SELECT count(*) FROM embedding_chunks WHERE vector IS NOT NULL)"); q.next();
    return {{"files",q.value(0)},{"passages",q.value(1)},{"vectors",q.value(2)}};
}
QVariantMap MetadataStore::preview(qint64 file, qint64 passage, int ordinal) const {
    auto document = execute(db_,"SELECT name,COALESCE(status,'pending'),(SELECT count(*) FROM passages WHERE file_id=files.id) FROM files LEFT JOIN documents ON documents.file_id=files.id WHERE files.id=?",{file});
    if (!document.next()) return {{"status","unavailable"},{"text",""},{"location",""},{"count",0},{"ordinal",0}};
    QVariantMap result{{"fileId",file},{"name",document.value(0)},{"status",document.value(1)},{"count",document.value(2)},{"ordinal",0},{"text",""},{"location",""}};
    QString condition; QVariantList values{file};
    if (ordinal >= 0) { condition = " AND ordinal=?"; values << ordinal; }
    else if (passage > 0) { condition = " AND id=?"; values << passage; }
    auto query = execute(db_,"SELECT text,location,ordinal FROM passages WHERE file_id=?"+condition+" ORDER BY ordinal LIMIT 1",values);
    if (query.next()) { result["text"] = query.value(0); result["location"] = query.value(1); result["ordinal"] = query.value(2); }
    return result;
}
}
