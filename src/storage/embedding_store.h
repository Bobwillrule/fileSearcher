#pragma once
#include "ai/model_assets.h"
#include <QSqlDatabase>
#include <functional>
#include <span>
namespace filesearcher {
struct EmbeddingPassage { qint64 id; QString text; };
struct PendingEmbedding { qint64 id; QString text; QByteArray hash; };
class EmbeddingStore {
public:
    explicit EmbeddingStore(const QString& database,const QString& model=modelFingerprint());
    ~EmbeddingStore();
    EmbeddingStore(const EmbeddingStore&)=delete;
    EmbeddingStore& operator=(const EmbeddingStore&)=delete;
    QList<EmbeddingPassage> pendingPassages(qint64 root) const;
    bool prepare(const EmbeddingPassage& passage,const QList<EmbeddingPart>& parts);
    QList<PendingEmbedding> pending(qint64 root,int limit) const;
    int commit(const QList<PendingEmbedding>& pending,const std::vector<std::vector<float>>& vectors);
    qint64 generation() const;
    qint64 count() const;
    void eachVector(const std::function<void(qint64,const std::vector<float>&)>& visitor) const;
    QString model() const { return model_; }
private:
    QString connection_,model_;
    QSqlDatabase db_;
};
}
