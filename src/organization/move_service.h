#pragma once
#include "organization/organization_store.h"
#include <QStringList>
#include <atomic>
#include <functional>

namespace filesearcher {
class MoveService {
public:
    MoveService(OrganizationStore& store,QStringList protectedFolders={},const std::atomic_bool* cancelled=nullptr);
    QJsonObject prepare(const QString& source,const QString& destinationFolder);
    QJsonObject prepareUndo(const QString& completedId);
    void cancel(const QString& id);
    void execute(const QString& id);
    void recover(); // Inspect only. Never resumes file mutations on startup.
    // Failure injection and same-disk exercise of the cross-volume protocol in tests.
    std::function<void(const QString&)> checkpoint;
    bool forceCopyForTesting=false;
private:
    void guard(const QString& path) const;
    void tick(const QString& stage) const;
    OrganizationStore& store_;
    QStringList protected_;
    const std::atomic_bool* cancelled_;
};
}
