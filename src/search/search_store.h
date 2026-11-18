#pragma once
#include "storage/metadata_store.h"
#include <QHash>
#include <span>
#include <vector>
namespace filesearcher {
struct SemanticRow { qint64 chunk; QVariantMap row; std::vector<float> vector; };
// Read-only connection, owned by the query thread. Every retrieval path uses the
// same metadata predicate and resolves vector IDs through current passage rows.
class SearchStore {
public:
    explicit SearchStore(const QString& database);
    ~SearchStore();
    SearchStore(const SearchStore&)=delete;
    SearchStore& operator=(const SearchStore&)=delete;
    QVariantList names(const SearchOptions& options) const;
    QVariantList keywords(const SearchOptions& options) const;
    std::pair<qint64,qint64> vectorCounts(const SearchOptions& options) const;
    bool hasVectors(const SearchOptions& options) const;
    qint64 generation() const;
    qint64 dataVersion() const;
    void vectors(const SearchOptions& options,const QList<qint64>& ids,
        const std::function<void(SemanticRow)>& visitor) const;
private:
    QString connection_;
    QSqlDatabase db_;
};
}
