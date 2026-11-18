#pragma once
#include "storage/metadata_store.h"
namespace filesearcher {
struct ContentReport { int processed = 0, warnings = 0, retries = 0; bool cancelled = false; };
ContentReport indexContents(MetadataStore& store, qint64 root, const QString& worker,
    const std::atomic_bool& cancelled, const std::function<void(int)>& progress = {});
}
