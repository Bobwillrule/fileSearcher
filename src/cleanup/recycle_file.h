#pragma once
#include <QString>
#include <functional>
#include <atomic>
namespace filesearcher {
// Throws on cancellation, unsupported recycling or a failed/unconfirmed operation.
// beforeDelete runs in the Shell's per-item callback, immediately before its action.
QString recycleFile(const QString& path,const std::function<void()>& beforeDelete,const std::atomic_bool& cancelled);
bool recyclingAllowed(unsigned long flags);
}
