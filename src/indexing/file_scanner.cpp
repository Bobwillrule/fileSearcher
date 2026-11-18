#include "indexing/file_scanner.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUuid>
#include <vector>
#include <stdexcept>
#define NOMINMAX
#include <windows.h>

namespace filesearcher {
namespace {
constexpr DWORD skippedAttributes = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_OFFLINE |
    FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
std::wstring native(const QString& path) {
    auto value = QDir::toNativeSeparators(normalizedPath(path));
    if (value.startsWith("\\\\?\\")) return value.toStdWString();
    return (value.startsWith("\\\\") ? "\\\\?\\UNC\\" + value.mid(2) : "\\\\?\\" + value).toStdWString();
}
struct FindHandle { HANDLE handle; ~FindHandle() { if (handle != INVALID_HANDLE_VALUE) FindClose(handle); } };
qint64 milliseconds(FILETIME time) {
    ULARGE_INTEGER value; value.LowPart = time.dwLowDateTime; value.HighPart = time.dwHighDateTime;
    return static_cast<qint64>(value.QuadPart / 10000) - 11644473600000LL;
}
}
QString validatedRoot(const QString& path) {
    const auto normalized = normalizedPath(path);
    const auto attributes = GetFileAttributesW(native(normalized).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("Choose an accessible local folder");
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("Linked folders and junctions are not supported as roots");
    if (normalized.startsWith("//")) throw std::runtime_error("Network folders are not supported in this phase");
    auto canonical = QFileInfo(normalized).canonicalFilePath();
    if (canonical.isEmpty()) throw std::runtime_error("Cannot resolve the selected folder");
    return normalizedPath(canonical);
}
QString directoryIdentity(const QString& path) {
    const auto handle = CreateFileW(native(path).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(handle,&info);
    CloseHandle(handle);
    if (!ok || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    return QString("%1:%2:%3").arg(info.dwVolumeSerialNumber).arg(info.nFileIndexHigh).arg(info.nFileIndexLow);
}
ScanReport scanRoot(MetadataStore& store, const IndexedRoot& root, const QStringList& protectedPaths,
                    const std::atomic_bool& cancelled, const std::function<void(qint64)>& progress) {
    ScanReport report;
    const auto generation = QUuid::createUuid().toString();
    QList<QRegularExpression> exclusions;
    for (const auto& pattern : root.exclusions) exclusions << QRegularExpression(QRegularExpression::wildcardToRegularExpression(pattern), QRegularExpression::CaseInsensitiveOption);
    const auto allowed = [&](const QString& path) {
        for (const auto& protectedPath : protectedPaths) if (withinPath(path, protectedPath)) return false;
        const auto relative = QDir(root.path).relativeFilePath(path);
        const auto name = QFileInfo(path).fileName();
        for (const auto& pattern : exclusions) if (pattern.match(name).hasMatch() || pattern.match(relative).hasMatch()) return false;
        return true;
    };
    store.rootStatus(root.id, "Scanning");
    store.beginScan();
    try {
        // Roots may have disappeared or become junctions since they were selected.
        validatedRoot(root.path);
        const auto identity = directoryIdentity(root.path);
        for (const auto& protectedPath : protectedPaths)
            if (withinPath(root.path, protectedPath)) throw std::runtime_error("The index data directory cannot be scanned");
        std::vector<QString> directories{root.path};
        while (!directories.empty() && !cancelled.load()) {
            const auto directory = directories.back(); directories.pop_back();
            const auto attributes = GetFileAttributesW(native(directory).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) { ++report.errors; continue; }
            WIN32_FIND_DATAW entry{};
            FindHandle finder{FindFirstFileExW(native(directory + "/*").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH)};
            if (finder.handle == INVALID_HANDLE_VALUE) {
                if (GetLastError() != ERROR_FILE_NOT_FOUND) ++report.errors;
                continue;
            }
            bool next = true;
            while (next && !cancelled.load()) {
                const auto name = QString::fromWCharArray(entry.cFileName);
                if (name != "." && name != "..") {
                    const auto path = normalizedPath(directory + '/' + name);
                    if ((entry.dwFileAttributes & skippedAttributes) || !allowed(path)) ++report.skipped;
                    else if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) directories.push_back(path);
                    else {
                        // Metadata only: locked/online-only files are never opened or hydrated.
                        WIN32_FILE_ATTRIBUTE_DATA fresh{};
                        if (!GetFileAttributesExW(native(path).c_str(), GetFileExInfoStandard, &fresh) ||
                            (fresh.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | skippedAttributes))) ++report.errors;
                        else {
                            const auto size = (static_cast<quint64>(fresh.nFileSizeHigh) << 32) | fresh.nFileSizeLow;
                            store.record(root.id, generation, {path,name,QFileInfo(name).suffix(),static_cast<qint64>(size),milliseconds(fresh.ftLastWriteTime)});
                            ++report.files;
                            if (progress && report.files % 100 == 0) progress(report.files);
                        }
                    }
                }
                next = FindNextFileW(finder.handle, &entry);
                if (!next && GetLastError() != ERROR_NO_MORE_FILES) ++report.errors;
            }
        }
        report.cancelled = cancelled.load();
        if (report.cancelled) { store.rollback(); report.message = "Cancelled; previous index preserved"; }
        else {
            // Never prune after a drive disappears or the selected directory is replaced mid-scan.
            validatedRoot(root.path);
            if (!identity.isEmpty() && identity != directoryIdentity(root.path))
                throw std::runtime_error("Folder changed during scan; previous index retained");
            store.finishScan(root.id, generation, report.errors == 0);
            report.message = report.errors ? "Partial scan; inaccessible records retained" : "Indexed";
        }
    } catch (const std::exception& error) { store.rollback(); ++report.errors; report.cancelled = cancelled.load(); report.message = QString::fromUtf8(error.what()); }
    store.rootStatus(root.id, report.message);
    if (progress) progress(report.files);
    return report;
}
}
