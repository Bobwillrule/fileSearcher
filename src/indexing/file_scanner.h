#pragma once
#include "storage/metadata_store.h"
namespace filesearcher {
struct ScanReport { qint64 files = 0, skipped = 0, errors = 0; bool cancelled = false; QString message; int contents = 0, contentWarnings = 0; QString aiStatus; };
QString validatedRoot(const QString& path);
QString directoryIdentity(const QString& path);
ScanReport scanRoot(MetadataStore& store, const IndexedRoot& root, const QStringList& protectedPaths,
                    const std::atomic_bool& cancelled, const std::function<void(qint64)>& progress = {});
}
