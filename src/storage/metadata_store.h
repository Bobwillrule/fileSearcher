#pragma once
#include <QSqlDatabase>
#include <QVariantList>
#include <QStringList>
#include <atomic>
#include <functional>
#include "extraction/content_types.h"

namespace filesearcher {
struct IndexedRoot { qint64 id; QString path; QStringList exclusions; QString status; };
struct FileMetadata { QString path; QString name; QString extension; qint64 size; qint64 modified; };
struct SearchOptions {
    QString query, extension;
    qint64 root = 0, minimumSize = -1, maximumSize = -1, modifiedAfter = -1;
    int limit = 500;
    int mode = 0; // 0 names, 1 names + content, 2 all terms, 3 phrase, 4 FTS syntax, 5 hybrid (search service)
};
struct ContentFile { qint64 id; QString path; qint64 size, modified; int attempts; };
struct SearchPage { QVariantList rows; qint64 total = 0; };
QString pathKey(const QString& path);
QString normalizedPath(const QString& path);
bool withinPath(const QString& path, const QString& parent);

// Own on one thread. Each worker constructs its own connection.
class MetadataStore {
public:
    explicit MetadataStore(const QString& database, bool initialize = false);
    ~MetadataStore();
    MetadataStore(const MetadataStore&) = delete;
    MetadataStore& operator=(const MetadataStore&) = delete;
    qint64 addRoot(const QString& path, const QStringList& exclusions);
    QList<IndexedRoot> roots() const;
    void updateExclusions(qint64 id, const QStringList& exclusions);
    void removeRoot(qint64 id);
    void rootStatus(qint64 id, const QString& status);
    void beginScan();
    void record(qint64 root, const QString& generation, const FileMetadata& file);
    void finishScan(qint64 root, const QString& generation, bool reconcile);
    void rollback();
    SearchPage search(const SearchOptions& options) const;
    QString filePath(qint64 id) const;
    QList<ContentFile> pendingContent(qint64 root, qint64 after = 0) const;
    bool replaceContent(const ContentFile& file, const QString& status, const QList<Passage>& passages);
    void resetContent();
    void clearSavedIndex(bool embeddingsOnly=false);
    QVariantMap statistics() const;
    QVariantMap preview(qint64 file, qint64 passage = 0, int ordinal = -1) const;
private:
    QString connection_;
    QSqlDatabase db_;
};
}
