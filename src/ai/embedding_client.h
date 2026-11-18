#pragma once
#include <QProcess>
#include <QJsonObject>
#include <QStringList>
#include <atomic>
#include <vector>
namespace filesearcher {
// One bounded, persistent inference process per indexing pass. UI never loads ORT.
class EmbeddingClient {
public:
    EmbeddingClient(const QString& executable,const QString& model,int threads,
                    const std::atomic_bool& cancelled,const std::atomic_bool& disabled);
    ~EmbeddingClient();
    std::vector<std::vector<float>> embed(const QStringList& texts,bool query=false);
    // Rebind only between completed requests, on the owning thread.
    void setCancellation(const std::atomic_bool& cancelled,const std::atomic_bool& disabled);
    qint64 processId() const { return process_.processId(); }
private:
    QJsonObject read();
    QProcess process_;
    QByteArray pending_;
    const std::atomic_bool* cancelled_;
    const std::atomic_bool* disabled_;
};
}
