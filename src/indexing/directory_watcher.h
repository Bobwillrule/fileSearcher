#pragma once
#include "storage/metadata_store.h"
#include <QObject>
#include <map>
#include <memory>

namespace filesearcher {
// One overlapped request per root, integrated with Qt's event loop; no thread per folder.
class DirectoryWatcher : public QObject {
    Q_OBJECT
public:
    explicit DirectoryWatcher(QObject* parent = nullptr);
    ~DirectoryWatcher() override;
    void synchronize(const QList<IndexedRoot>& roots, const QStringList& protectedPaths);
    QStringList unavailable() const;
signals:
    // Empty/overflow notifications use the same full reconciliation path.
    void dirty(qint64 root);
    void overflowed(qint64 root);
    void coverageChanged();
private:
    struct Watch;
    std::map<qint64,std::unique_ptr<Watch>> watches_;
    QStringList unavailable_;
    void complete(qint64 root);
};
}
