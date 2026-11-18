#pragma once
#include <QString>
#include <atomic>
#include <functional>
namespace filesearcher {
struct EmbeddingReport { int embedded=0; qint64 total=0; bool rebuilt=false,cancelled=false; QString status; };
EmbeddingReport indexEmbeddings(const QString& database,qint64 root,const QString& worker,const QString& model,
    const QString& cache,int profile,const std::atomic_bool& cancelled,const std::atomic_bool& disabled,
    const std::function<void(int)>& progress={});
}
