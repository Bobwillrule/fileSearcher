#pragma once
#include <QString>
#include <atomic>
#include "extraction/content_types.h"

namespace filesearcher {
struct ProbeResult {
    QString status;
    QString text;
    QString detail;
    QList<TextSection> sections;
};
// Blocking, bounded worker protocol. Call only from a background thread.
ProbeResult runExtractionProbe(const QString& executable, const QString& input, int timeout_ms = 5000, const std::atomic_bool* cancelled = nullptr);
}
