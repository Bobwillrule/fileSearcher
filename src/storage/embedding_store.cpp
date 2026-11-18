#include "storage/embedding_store.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QUuid>
#include <QCryptographicHash>
#include <QtEndian>
#include <bit>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace filesearcher {
namespace {
QSqlQuery sql(QSqlDatabase db,const QString& text,const QVariantList& values={}) {
    QSqlQuery query(db);
    if (!query.prepare(text)) throw std::runtime_error(query.lastError().text().toStdString());
    for (const auto& value:values) query.addBindValue(value);
    if (!query.exec()) throw std::runtime_error(query.lastError().text().toStdString());
    return query;
}
void validate(std::span<const float> vector) {
    if (vector.size()!=384) throw std::runtime_error("Invalid stored vector dimensions");
    double squared=0; for (float f:vector) squared+=static_cast<double>(f)*f;
    if (!std::isfinite(squared) || std::abs(squared-1)>0.001) throw std::runtime_error("Invalid stored vector norm");
}
QByteArray pack(std::span<const float> vector) {
    validate(vector); QByteArray bytes(384*4,Qt::Uninitialized);
    for (size_t i=0;i<vector.size();++i) qToLittleEndian(std::bit_cast<quint32>(vector[i]),bytes.data()+i*4);
    return bytes;
}
std::vector<float> unpack(const QByteArray& bytes) {
    if (bytes.size()!=384*4) throw std::runtime_error("Invalid stored vector length");
    std::vector<float> vector(384);
    for (int i=0;i<384;++i) vector[i]=std::bit_cast<float>(qFromLittleEndian<quint32>(bytes.data()+i*4));
    validate(vector); return vector;
}
}
EmbeddingStore::EmbeddingStore(const QString& database,const QString& model):model_(model) {
    connection_=QUuid::createUuid().toString(); db_=QSqlDatabase::addDatabase("QSQLITE",connection_);
    db_.setDatabaseName(database); db_.setConnectOptions("QSQLITE_BUSY_TIMEOUT=2000");
    try {
        if (!db_.open()) throw std::runtime_error("Cannot open embedding database");
        sql(db_,"PRAGMA foreign_keys=ON");
        sql(db_,"INSERT INTO embedding_models(fingerprint) VALUES(?) ON CONFLICT DO NOTHING",{model_});
    } catch (...) { db_.close(); db_={}; QSqlDatabase::removeDatabase(connection_); throw; }
}
EmbeddingStore::~EmbeddingStore() { db_.close(); db_={}; QSqlDatabase::removeDatabase(connection_); }
QList<EmbeddingPassage> EmbeddingStore::pendingPassages(qint64 root) const {
    auto query=sql(db_,"SELECT p.id,p.text FROM passages p JOIN root_files rf ON rf.file_id=p.file_id WHERE rf.root_id=? AND NOT EXISTS(SELECT 1 FROM embedding_chunks e WHERE e.passage_id=p.id AND e.model=?) ORDER BY p.id LIMIT 32",{root,model_});
    QList<EmbeddingPassage> result; while (query.next()) result << EmbeddingPassage{query.value(0).toLongLong(),query.value(1).toString()}; return result;
}
bool EmbeddingStore::prepare(const EmbeddingPassage& passage,const QList<EmbeddingPart>& parts) {
    if (parts.isEmpty()) throw std::runtime_error("Empty embedding partition");
    int covered=0;
    for (const auto& part:parts) {
        if (part.offset!=covered || part.length<=0 || part.text!=passage.text.mid(part.offset,part.length) || part.tokens<2 || part.tokens>386)
            throw std::runtime_error("Invalid embedding partition");
        covered+=part.length;
    }
    if (covered!=passage.text.size()) throw std::runtime_error("Incomplete embedding partition");
    if (!db_.transaction()) throw std::runtime_error("Cannot prepare embedding transaction");
    try {
        auto current=sql(db_,"SELECT text FROM passages WHERE id=?",{passage.id});
        if (!current.next() || current.value(0).toString()!=passage.text) { current.finish(); db_.rollback(); return false; }
        current.finish();
        int ordinal=0;
        for (const auto& part:parts) sql(db_,"INSERT INTO embedding_chunks(passage_id,model,ordinal,start_offset,length,text_hash,tokens) VALUES(?,?,?,?,?,?,?) ON CONFLICT(passage_id,model,ordinal) DO NOTHING",
            {passage.id,model_,ordinal++,part.offset,part.length,QCryptographicHash::hash(part.text.toUtf8(),QCryptographicHash::Sha256),part.tokens});
        if (!db_.commit()) throw std::runtime_error("Cannot commit embedding preparation"); return true;
    } catch (...) { db_.rollback(); throw; }
}
QList<PendingEmbedding> EmbeddingStore::pending(qint64 root,int limit) const {
    auto query=sql(db_,"SELECT e.id,p.text,e.start_offset,e.length,e.text_hash FROM embedding_chunks e JOIN passages p ON p.id=e.passage_id JOIN root_files rf ON rf.file_id=p.file_id WHERE rf.root_id=? AND e.model=? AND e.vector IS NULL ORDER BY e.id LIMIT ?",{root,model_,std::clamp(limit,1,8)});
    QList<PendingEmbedding> result;
    while (query.next()) {
        const auto text=query.value(1).toString().mid(query.value(2).toInt(),query.value(3).toInt());
        const auto hash=query.value(4).toByteArray();
        if (QCryptographicHash::hash(text.toUtf8(),QCryptographicHash::Sha256)!=hash) throw std::runtime_error("Embedding source mismatch");
        result << PendingEmbedding{query.value(0).toLongLong(),text,hash};
    }
    return result;
}
int EmbeddingStore::commit(const QList<PendingEmbedding>& pending,const std::vector<std::vector<float>>& vectors) {
    if (pending.size()!=vectors.size() || pending.size()>8) throw std::runtime_error("Invalid vector batch");
    if (!db_.transaction()) throw std::runtime_error("Cannot save embeddings");
    try {
        int saved=0;
        for (qsizetype i=0;i<pending.size();++i) {
            auto query=sql(db_,"UPDATE embedding_chunks SET vector=? WHERE id=? AND model=? AND text_hash=? AND vector IS NULL",{pack(vectors[i]),pending[i].id,model_,pending[i].hash});
            saved+=query.numRowsAffected();
        }
        if (!db_.commit()) throw std::runtime_error("Cannot commit embeddings"); return saved;
    } catch (...) { db_.rollback(); throw; }
}
qint64 EmbeddingStore::generation() const { auto q=sql(db_,"SELECT generation FROM embedding_models WHERE fingerprint=?",{model_}); q.next(); return q.value(0).toLongLong(); }
qint64 EmbeddingStore::count() const { auto q=sql(db_,"SELECT count(*) FROM embedding_chunks WHERE model=? AND vector IS NOT NULL",{model_}); q.next(); return q.value(0).toLongLong(); }
void EmbeddingStore::eachVector(const std::function<void(qint64,const std::vector<float>&)>& visitor) const {
    auto query=sql(db_,"SELECT id,vector FROM embedding_chunks WHERE model=? AND vector IS NOT NULL ORDER BY id",{model_});
    while (query.next()) visitor(query.value(0).toLongLong(),unpack(query.value(1).toByteArray()));
}
}
