#include "indexing/work_queue.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QUuid>
#include <QVariant>
#include <stdexcept>

namespace filesearcher {
namespace {
QSqlQuery exec(QSqlDatabase db, const QString& sql, const QVariantList& values = {}) {
    QSqlQuery query(db);
    if (!query.prepare(sql)) throw std::runtime_error(query.lastError().text().toStdString());
    for (const auto& value : values) query.addBindValue(value);
    if (!query.exec()) throw std::runtime_error(query.lastError().text().toStdString());
    return query;
}
}
WorkQueue::WorkQueue(const QString& database) {
    connection_ = QUuid::createUuid().toString();
    db_ = QSqlDatabase::addDatabase("QSQLITE",connection_); db_.setDatabaseName(database);
    db_.setConnectOptions("QSQLITE_BUSY_TIMEOUT=2000");
    try {
        if (!db_.open()) throw std::runtime_error(db_.lastError().text().toStdString());
        exec(db_,"PRAGMA journal_mode=WAL");
        exec(db_,"PRAGMA synchronous=FULL");
        exec(db_,"CREATE TABLE IF NOT EXISTS work(root INTEGER PRIMARY KEY,path TEXT NOT NULL,revision INTEGER NOT NULL,due INTEGER NOT NULL,attempts INTEGER NOT NULL DEFAULT 0,running INTEGER NOT NULL DEFAULT 0)");
        exec(db_,"CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value INTEGER NOT NULL)");
        // A process may have stopped after claiming work, or after publishing metadata.
        // Repeating reconciliation is idempotent in both cases.
        exec(db_,"UPDATE work SET running=0");
    } catch (...) { db_.close(); db_ = {}; QSqlDatabase::removeDatabase(connection_); throw; }
}
WorkQueue::~WorkQueue() { db_.close(); db_ = {}; QSqlDatabase::removeDatabase(connection_); }
void WorkQueue::enqueue(qint64 root, const QString& path, qint64 now, int delayMs) {
    exec(db_,"INSERT INTO work(root,path,revision,due) VALUES(?,?,1,?) ON CONFLICT(root) DO UPDATE SET path=excluded.path,revision=work.revision+1,due=MIN(work.due,excluded.due),attempts=0",{root,path,now+delayMs});
}
std::optional<IndexWork> WorkQueue::take(qint64 now) {
    auto query = exec(db_,"SELECT root,path,revision,attempts FROM work WHERE running=0 AND attempts<3 AND due<=? ORDER BY due,root LIMIT 1",{now});
    if (!query.next()) return {};
    IndexWork work{query.value(0).toLongLong(),query.value(1).toString(),query.value(2).toLongLong(),query.value(3).toInt()};
    query.finish(); exec(db_,"UPDATE work SET running=1 WHERE root=?",{work.root}); return work;
}
void WorkQueue::finish(const IndexWork& work, bool success, bool cancelled, qint64 now) {
    if (success) exec(db_,"DELETE FROM work WHERE root=? AND revision=?",{work.root,work.revision});
    else if (cancelled) exec(db_,"UPDATE work SET running=0 WHERE root=? AND revision=?",{work.root,work.revision});
    else exec(db_,"UPDATE work SET running=0,attempts=attempts+1,due=? WHERE root=? AND revision=?",{now+(1000LL << work.attempts),work.root,work.revision});
    // Events arriving during the scan always require a follow-up pass.
    exec(db_,"UPDATE work SET running=0 WHERE root=? AND revision<>?",{work.root,work.revision});
}
void WorkQueue::remove(qint64 root) { exec(db_,"DELETE FROM work WHERE root=?",{root}); }
void WorkQueue::retain(const QList<qint64>& roots) {
    auto query = exec(db_,"SELECT root FROM work"); QList<qint64> stale;
    while (query.next()) if (!roots.contains(query.value(0).toLongLong())) stale << query.value(0).toLongLong();
    query.finish(); for (auto id : stale) remove(id);
}
int WorkQueue::pending() const { auto query = exec(db_,"SELECT count(*) FROM work"); query.next(); return query.value(0).toInt(); }
bool WorkQueue::paused() const { auto query = exec(db_,"SELECT value FROM settings WHERE key='paused'"); return query.next() && query.value(0).toBool(); }
void WorkQueue::setPaused(bool paused) { exec(db_,"INSERT INTO settings(key,value) VALUES('paused',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",{paused}); }
}
