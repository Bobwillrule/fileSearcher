#include "indexing/directory_watcher.h"
#include "indexing/file_scanner.h"
#include <QDir>
#include <QRegularExpression>
#include <QWinEventNotifier>
#include <array>
#include <algorithm>
#include <cstddef>
#define NOMINMAX
#include <windows.h>

namespace filesearcher {
struct DirectoryWatcher::Watch {
    IndexedRoot root;
    QString identity;
    QStringList protectedPaths;
    QList<QRegularExpression> exclusions;
    HANDLE directory = INVALID_HANDLE_VALUE, event = nullptr;
    OVERLAPPED request{};
    alignas(DWORD) std::array<char,64*1024> buffer{};
    std::unique_ptr<QWinEventNotifier> notifier;
    bool active = false;
    ~Watch() {
        notifier.reset();
        if (directory != INVALID_HANDLE_VALUE) {
            if (active) { CancelIoEx(directory,&request); DWORD ignored; GetOverlappedResult(directory,&request,&ignored,TRUE); }
            CloseHandle(directory);
        }
        if (event) CloseHandle(event);
    }
    bool arm() {
        ResetEvent(event); request = {}; request.hEvent = event;
        active = ReadDirectoryChangesW(directory,buffer.data(),static_cast<DWORD>(buffer.size()),TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SECURITY,
            nullptr,&request,nullptr);
        return active;
    }
    bool relevant(const QString& relative) const {
        const auto path = normalizedPath(root.path + '/' + relative);
        if (!withinPath(path,root.path)) return true; // Unexpected record: reconcile safely.
        for (const auto& protectedPath : protectedPaths) if (withinPath(path,protectedPath)) return false;
        QString prefix;
        for (const auto& segment : QDir::fromNativeSeparators(relative).split('/')) {
            prefix = prefix.isEmpty() ? segment : prefix + '/' + segment;
            for (const auto& pattern : exclusions)
                if (pattern.match(segment).hasMatch() || pattern.match(prefix).hasMatch()) return false;
        }
        return true;
    }
};
DirectoryWatcher::DirectoryWatcher(QObject* parent) : QObject(parent) {}
DirectoryWatcher::~DirectoryWatcher() = default;
void DirectoryWatcher::synchronize(const QList<IndexedRoot>& roots, const QStringList& protectedPaths) {
    for (auto it = watches_.begin(); it != watches_.end();) {
        const auto found = std::find_if(roots.begin(),roots.end(),[&](const auto& root) { return root.id == it->first && root.path == it->second->root.path && root.exclusions == it->second->root.exclusions; });
        if (found == roots.end() || !it->second->active || (!it->second->identity.isEmpty() && it->second->identity != directoryIdentity(found->path))) it = watches_.erase(it); else ++it;
    }
    unavailable_.clear();
    for (const auto& root : roots) {
        if (watches_.contains(root.id)) continue;
        // Keep kernel resources bounded. Extra roots still receive periodic reconciliation.
        if (watches_.size() >= 128) { unavailable_ << root.path; continue; }
        auto watch = std::make_unique<Watch>(); watch->root = root; watch->protectedPaths = protectedPaths;
        for (const auto& pattern : root.exclusions) watch->exclusions << QRegularExpression(QRegularExpression::wildcardToRegularExpression(pattern),QRegularExpression::CaseInsensitiveOption);
        try { validatedRoot(root.path); } catch (...) { unavailable_ << root.path; continue; }
        watch->identity = directoryIdentity(root.path);
        auto path = QDir::toNativeSeparators(root.path);
        if (!path.startsWith("\\\\?\\")) path = "\\\\?\\" + path;
        watch->directory = CreateFileW(path.toStdWString().c_str(),FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        watch->event = CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if (watch->directory == INVALID_HANDLE_VALUE || !watch->event || !watch->arm()) { unavailable_ << root.path; continue; }
        watch->notifier = std::make_unique<QWinEventNotifier>(watch->event);
        connect(watch->notifier.get(),&QWinEventNotifier::activated,this,[this,id=root.id] { complete(id); });
        watches_[root.id] = std::move(watch);
    }
    emit coverageChanged();
}
QStringList DirectoryWatcher::unavailable() const { return unavailable_; }
void DirectoryWatcher::complete(qint64 root) {
    auto it = watches_.find(root); if (it == watches_.end()) return;
    auto& watch = *it->second; watch.notifier->setEnabled(false);
    DWORD bytes = 0;
    const bool success = GetOverlappedResult(watch.directory,&watch.request,&bytes,FALSE);
    const bool overflow = (success && bytes == 0) || (!success && GetLastError() == ERROR_NOTIFY_ENUM_DIR);
    watch.active = false;
    bool changed = !success || bytes == 0;
    // Bound every record before reading the variable-length name.
    for (DWORD offset = 0; success && offset < bytes;) {
        constexpr DWORD header = offsetof(FILE_NOTIFY_INFORMATION,FileName);
        if (bytes-offset < header) { changed = true; break; }
        const auto* record = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(watch.buffer.data()+offset);
        if (record->FileNameLength % sizeof(wchar_t) || record->FileNameLength > bytes-offset-header) { changed = true; break; }
        if (watch.relevant(QString::fromWCharArray(record->FileName,record->FileNameLength/sizeof(wchar_t)))) changed = true;
        if (!record->NextEntryOffset) break;
        if (record->NextEntryOffset < header+record->FileNameLength || record->NextEntryOffset % alignof(DWORD) || record->NextEntryOffset >= bytes-offset) { changed = true; break; }
        offset += record->NextEntryOffset;
    }
    if (success && watch.arm()) watch.notifier->setEnabled(true);
    else { unavailable_ << watch.root.path; changed = true; emit coverageChanged(); }
    if (overflow) emit overflowed(root);
    if (changed) emit dirty(root);
}
}
