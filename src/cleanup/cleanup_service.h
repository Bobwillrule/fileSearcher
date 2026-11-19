#pragma once
#include "organization/organization_store.h"
#include <atomic>
#include <functional>
namespace filesearcher {
struct CleanupScan { QVariantList candidates; int inspected=0,skipped=0; bool limited=false; };
class CleanupService {
public:
    CleanupService(OrganizationStore& store,QStringList protectedFolders,const std::atomic_bool& cancelled);
    CleanupScan scan(const QString& metadata,qint64 root,bool installers,bool builds);
    QJsonObject prepare(QJsonObject candidate,bool reverse=false);
    QJsonObject execute(const QString& id);
    void cancel(const QString& id);
    void recover();
    QVariantList history() const;
    QJsonObject operation(const QString& id) const;
    // Checkpoints inject failures only; production always uses the real recycler.
    std::function<void(const QString&)> checkpoint;
private:
    void put(const QJsonObject& op);
    OrganizationStore& store_;
    QStringList protected_;
    const std::atomic_bool& cancelled_;
};
}
