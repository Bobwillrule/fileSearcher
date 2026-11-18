#pragma once
#include <QSqlDatabase>
#include <QString>
#include <optional>

namespace filesearcher {
struct IndexWork { qint64 root = 0; QString path; qint64 revision = 0; int attempts = 0; };
// Separate from metadata.sqlite so events remain durable while a scan owns its write transaction.
class WorkQueue {
public:
    explicit WorkQueue(const QString& database);
    ~WorkQueue();
    WorkQueue(const WorkQueue&) = delete;
    WorkQueue& operator=(const WorkQueue&) = delete;
    void enqueue(qint64 root, const QString& path, qint64 now, int delayMs = 500);
    std::optional<IndexWork> take(qint64 now);
    void finish(const IndexWork& work, bool success, bool cancelled, qint64 now);
    void remove(qint64 root);
    void retain(const QList<qint64>& roots);
    int pending() const;
    bool paused() const;
    void setPaused(bool paused);
private:
    QString connection_;
    QSqlDatabase db_;
};
}
