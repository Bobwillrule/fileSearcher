#pragma once
#include "organization/organization_store.h"
#include <atomic>
namespace filesearcher {
QVariantMap organizationFile(const QString& database,qint64 file);
QVariantList relatedFiles(const QString& database,const QString& cache,qint64 file,const std::atomic_bool& cancelled,bool useSemantics=true);
QVariantList categorySuggestions(const QString& database,OrganizationStore& organization,qint64 file,const std::atomic_bool& cancelled,bool useSemantics=true);
}
