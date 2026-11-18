#include "search/search_store.h"
#include "ai/model_assets.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QRegularExpression>
#include <QSet>
#include <QDir>
#include <QUuid>
#include <QtEndian>
#include <bit>
#include <cmath>
#include <stdexcept>
namespace filesearcher {
namespace {
QSqlQuery sql(QSqlDatabase db,const QString& text,const QVariantList& values={}) {
    QSqlQuery q(db); if (!q.prepare(text)) throw std::runtime_error(q.lastError().text().toStdString());
    for (const auto& v:values) q.addBindValue(v);
    if (!q.exec()) throw std::runtime_error(q.lastError().text().toStdString()); return q;
}
QString filter(const SearchOptions& o,QVariantList& values) {
    QString result;
    auto extension=o.extension.trimmed().toCaseFolded(); if (extension.startsWith('.')) extension.remove(0,1);
    if (!extension.isEmpty()) { result+=" AND files.extension=?"; values<<extension; }
    if (o.root>0) { result+=" AND EXISTS(SELECT 1 FROM root_files WHERE file_id=files.id AND root_id=?)"; values<<o.root; }
    if (o.minimumSize>=0) { result+=" AND files.size>=?"; values<<o.minimumSize; }
    if (o.maximumSize>=0) { result+=" AND files.size<=?"; values<<o.maximumSize; }
    if (o.modifiedAfter>=0) { result+=" AND files.modified>=?"; values<<o.modifiedAfter; }
    return result;
}
const QString columns="files.id,files.path,files.name,files.extension,files.size,files.modified,COALESCE(documents.status,'pending')";
const QString joins=" FROM files LEFT JOIN documents ON documents.file_id=files.id";
QVariantMap row(QSqlQuery& q) {
    return {{"fileId",q.value(0)},{"path",q.value(1)},{"name",q.value(2)},{"extension",q.value(3)},
        {"size",q.value(4)},{"modified",q.value(5)},{"contentStatus",q.value(6)},
        {"passageId",q.value(7)},{"snippet",q.value(8)},{"location",q.value(9)}};
}
QString literal(QString s) { return s.replace("!","!!").replace("%","!%").replace("_","!_"); }
QString expression(const QString& text) {
    static const QSet<QString> stop{"a","an","and","are","as","at","be","by","can","do","does","for","from","how","i","in","is","it","me","my","of","on","or","please","show","that","the","these","this","to","was","what","where","which","with","you","find","file","files","document","documents"};
    QStringList words; QSet<QString> seen;
    static const QRegularExpression token("[\\p{L}\\p{N}_]+",QRegularExpression::UseUnicodePropertiesOption);
    auto matches=token.globalMatch(text.toCaseFolded());
    while (matches.hasNext()) {
        const auto word=matches.next().captured();
        if (!stop.contains(word) && !seen.contains(word)) { words << '"'+word+'"'; seen.insert(word); }
        if (words.size()>=64) break;
    }
    return words.join(" OR ");
}
}
SearchStore::SearchStore(const QString& database) {
    connection_=QUuid::createUuid().toString(); db_=QSqlDatabase::addDatabase("QSQLITE",connection_);
    db_.setDatabaseName(database); db_.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=2000");
    if (!db_.open()) { auto message=db_.lastError().text(); db_={}; QSqlDatabase::removeDatabase(connection_); throw std::runtime_error(message.toStdString()); }
}
SearchStore::~SearchStore() { db_.close(); db_={}; QSqlDatabase::removeDatabase(connection_); }
QVariantList SearchStore::names(const SearchOptions& o) const {
    const auto text=QDir::fromNativeSeparators(o.query.trimmed()).toCaseFolded();
    QVariantList values{'%'+literal(text)+'%','%'+literal(text)+'%'};
    const auto where=filter(o,values); values<<text<<text;
    auto q=sql(db_,"SELECT "+columns+",0,'',''"+joins+" WHERE (files.name_key LIKE ? ESCAPE '!' OR files.path_key LIKE ? ESCAPE '!')"+where+
        " ORDER BY CASE WHEN files.name_key=? THEN 0 WHEN files.path_key=? THEN 1 ELSE 2 END,files.name_key,files.id LIMIT 200",values);
    QVariantList result; while(q.next()) result<<row(q); return result;
}
QVariantList SearchStore::keywords(const SearchOptions& o) const {
    const auto terms=expression(o.query); if (terms.isEmpty()) return {};
    QVariantList values{terms}; const auto where=filter(o,values);
    // Rank all matching passages, but generate snippets only for the 200 winning files.
    values<<terms;
    auto q=sql(db_,"WITH hits AS MATERIALIZED (SELECT rowid AS passage,bm25(passages_fts) AS score FROM passages_fts WHERE passages_fts MATCH ?), "
        "ranked AS (SELECT passages.file_id,passages.id,passages.location,hits.score,row_number() OVER(PARTITION BY passages.file_id ORDER BY hits.score,passages.id) AS position FROM hits JOIN passages ON passages.id=hits.passage), "
        "winners AS MATERIALIZED (SELECT files.id AS fileId,files.path,files.name,files.extension,files.size,files.modified,COALESCE(documents.status,'pending') AS status,ranked.id AS passage,ranked.location,ranked.score"+joins+
        " JOIN ranked ON ranked.file_id=files.id AND ranked.position=1 WHERE 1=1"+where+" ORDER BY ranked.score,files.id LIMIT 200) "
        "SELECT winners.fileId,winners.path,winners.name,winners.extension,winners.size,winners.modified,winners.status,winners.passage,snippet(passages_fts,0,'[',']',' ... ',32),winners.location "
        "FROM winners JOIN passages_fts ON passages_fts.rowid=winners.passage WHERE passages_fts MATCH ? ORDER BY winners.score,winners.fileId",values);
    QVariantList result; while(q.next()) result<<row(q); return result;
}
std::pair<qint64,qint64> SearchStore::vectorCounts(const SearchOptions& o) const {
    QVariantList values{modelFingerprint()}; auto where=filter(o,values);
    auto q=sql(db_,"SELECT count(*),count(DISTINCT files.id) FROM embedding_chunks e JOIN passages p ON p.id=e.passage_id JOIN files ON files.id=p.file_id WHERE e.model=? AND e.vector IS NOT NULL"+where,values);
    q.next(); return {q.value(0).toLongLong(),q.value(1).toLongLong()};
}
bool SearchStore::hasVectors(const SearchOptions& o) const {
    QVariantList values{modelFingerprint()}; const auto where=filter(o,values);
    auto q=sql(db_,"SELECT 1 FROM embedding_chunks e JOIN passages p ON p.id=e.passage_id JOIN files ON files.id=p.file_id WHERE e.model=? AND e.vector IS NOT NULL"+where+" LIMIT 1",values);
    return q.next();
}
qint64 SearchStore::generation() const { auto q=sql(db_,"SELECT generation FROM embedding_models WHERE fingerprint=?",{modelFingerprint()}); return q.next()?q.value(0).toLongLong():0; }
qint64 SearchStore::dataVersion() const { auto q=sql(db_,"PRAGMA data_version"); q.next(); return q.value(0).toLongLong(); }
void SearchStore::vectors(const SearchOptions& o,const QList<qint64>& ids,const std::function<void(SemanticRow)>& visitor) const {
    QVariantList values{modelFingerprint()}; auto where=filter(o,values);
    if (!ids.isEmpty()) { QStringList marks; for (auto id:ids) { marks<<"?"; values<<id; } where+=" AND e.id IN ("+marks.join(',')+")"; }
    auto q=sql(db_,"SELECT "+columns+",p.id,p.text,p.location,e.id,e.vector,e.start_offset,e.length"+joins+
        " JOIN passages p ON p.file_id=files.id JOIN embedding_chunks e ON e.passage_id=p.id WHERE e.model=? AND e.vector IS NOT NULL"+where+" ORDER BY e.id",values);
    while (q.next()) {
        const auto bytes=q.value(11).toByteArray(); if (bytes.size()!=1536) throw std::runtime_error("Invalid stored vector");
        std::vector<float> vector(384); double norm=0;
        for (int i=0;i<384;++i) { vector[i]=std::bit_cast<float>(qFromLittleEndian<quint32>(bytes.data()+i*4)); norm+=static_cast<double>(vector[i])*vector[i]; }
        if (!std::isfinite(norm) || std::abs(norm-1)>0.001) throw std::runtime_error("Invalid stored vector");
        auto result=row(q); const auto text=result["snippet"].toString().mid(q.value(12).toInt(),q.value(13).toInt());
        result["snippet"]=text.left(320)+(text.size()>320?QStringLiteral(" …"):QString()); result["chunkId"]=q.value(10);
        visitor({q.value(10).toLongLong(),std::move(result),std::move(vector)});
    }
}
}
