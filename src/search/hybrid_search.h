#pragma once
#include "storage/metadata_store.h"
#include <span>
#include <vector>
#include <memory>
namespace filesearcher {
struct HybridResult { SearchPage page; QString notice; bool semantic=false; QString semanticPath; };
struct SearchResources;
// Exposed separately so ranking/filters can be tested independently of inference.
HybridResult retrieveHybrid(const QString& database,const QString& cache,const SearchOptions& options,
    std::span<const float> queryVector,const std::atomic_bool& cancelled,SearchResources* resources=nullptr);
HybridResult searchHybrid(const QString& database,const QString& cache,const QString& worker,const QString& model,
    const SearchOptions& options,bool aiEnabled,const std::atomic_bool& cancelled);
// Construct, use and destroy on one dedicated query thread. Retains a verified
// model process and generation-bound graph, never query text or result lists.
class SearchSession {
public:
    SearchSession(QString database,QString cache,QString worker,QString model);
    ~SearchSession();
    HybridResult search(const SearchOptions& options,bool enabled,std::shared_ptr<std::atomic_bool> cancelled);
    void release();
    int workerStarts() const;
    qint64 workerPid() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
