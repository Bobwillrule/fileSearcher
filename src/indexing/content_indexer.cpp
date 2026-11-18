#include "indexing/content_indexer.h"
#include "extraction/probe_runner.h"
#include <QFileInfo>
#include <QDateTime>
#include <QSet>

namespace filesearcher {
ContentReport indexContents(MetadataStore& store, qint64 root, const QString& worker,
    const std::atomic_bool& cancelled, const std::function<void(int)>& progress) {
    ContentReport report; qint64 after = 0;
    static const QSet<QString> retryable{"read_failed","timeout","worker_failed","start_failed","changed_during_read","missing_file"};
    while (!cancelled.load()) {
        const auto files = store.pendingContent(root,after); if (files.isEmpty()) break;
        for (const auto& file : files) {
            if (cancelled.load()) break;
            after = file.id;
            QFileInfo info(file.path);
            const auto extension = info.suffix().toLower();
            ProbeResult result;
            if (!isTextExtension(extension) && extension != "pdf" && extension != "docx") result.status = "unsupported_format";
            else if (!info.isFile()) result.status = "missing_file";
            else if (info.size() != file.size || info.lastModified().toMSecsSinceEpoch() != file.modified) result.status = "changed_during_read";
            else result = runExtractionProbe(worker,file.path,5000,&cancelled);
            if (cancelled.load() || result.status == "cancelled") { report.cancelled = true; break; }
            info.refresh();
            if (info.exists() && (info.size() != file.size || info.lastModified().toMSecsSinceEpoch() != file.modified)) {
                result = {"changed_during_read",{}, {}};
            } else if (!info.exists()) result = {"missing_file",{}, {}};
            const bool searchable = result.status == "ok" || result.status == "text_limit";
            const auto passages = searchable ? splitPassages(result.sections) : QList<Passage>();
            if (cancelled.load()) { report.cancelled = true; break; }
            if (!store.replaceContent(file,result.status,passages)) ++report.retries;
            else {
                ++report.processed;
                if (result.status != "ok") ++report.warnings;
                if (retryable.contains(result.status) && file.attempts < 2) ++report.retries;
            }
            if (progress) progress(report.processed);
        }
    }
    report.cancelled = report.cancelled || cancelled.load();
    return report;
}
}
