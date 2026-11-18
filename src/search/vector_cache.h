#pragma once
#include "search/vector_index.h"
#include "storage/embedding_store.h"
#include <atomic>
namespace filesearcher {
struct CachedVectors { std::unique_ptr<VectorIndex> index; bool rebuilt=false; qint64 generation=0; };
// Only use returned graph while its generation still matches SQLite. Hybrid search
// uses a read-only loader and resolves IDs against current, filtered passage rows.
CachedVectors recoverVectorCache(EmbeddingStore& store,const QString& directory,
    const std::atomic_bool& cancelled,const std::atomic_bool& disabled);
}
