#include "cleanup/file_checks.h"
#include "storage/metadata_store.h"
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <stdexcept>

namespace filesearcher::cleanup {
namespace {
void check(bool ok,const char* message) { if(!ok) throw std::runtime_error(QString("%1 (Windows error %2)").arg(message).arg(GetLastError()).toStdString()); }
std::wstring native(const QString& path) { return ("\\\\?\\"+QDir::toNativeSeparators(path)).toStdWString(); }
Held handle(const QString& path,DWORD access,DWORD share,bool directory) {
    auto file=std::make_unique<Handle>(CreateFileW(native(path).c_str(),access,share,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:FILE_FLAG_SEQUENTIAL_SCAN),nullptr));
    check(file->value!=INVALID_HANDLE_VALUE,"File or folder is unavailable or in use");
    BY_HANDLE_FILE_INFORMATION i{}; check(GetFileInformationByHandle(file->value,&i),"Cannot inspect file");
    constexpr DWORD excluded=FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_OFFLINE|FILE_ATTRIBUTE_RECALL_ON_OPEN|FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS|FILE_ATTRIBUTE_ENCRYPTED;
    if((i.dwFileAttributes&excluded) || bool(i.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=directory) throw std::runtime_error("Links, cloud placeholders and encrypted files are excluded");
    if(!directory && (i.nNumberOfLinks!=1 || (i.dwFileAttributes&FILE_ATTRIBUTE_READONLY))) throw std::runtime_error("Hard links and read-only files are excluded; no reclaimable space is claimed for them");
    std::vector<wchar_t> buffer(32768); auto n=GetFinalPathNameByHandleW(file->value,buffer.data(),DWORD(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    check(n && n<buffer.size(),"Cannot resolve file location");
    if(QString::compare(QString::fromWCharArray(buffer.data(),n),QString::fromStdWString(native(path)),Qt::CaseInsensitive)) throw std::runtime_error("Redirected paths are excluded");
    return file;
}
void rewind(HANDLE f) { LARGE_INTEGER zero{}; check(SetFilePointerEx(f,zero,nullptr,FILE_BEGIN),"Cannot read file"); }
DWORD read(HANDLE f,QByteArray& b) { DWORD n=0; check(ReadFile(f,b.data(),DWORD(b.size()),&n,nullptr),"Cannot read file"); return n; }
QString ticks(FILETIME t) { return QString::number((quint64(t.dwHighDateTime)<<32)|t.dwLowDateTime); }
bool buildPath(const QString& path) {
    const auto parts=path.toCaseFolded().split('/');
    return parts.contains("build") || parts.contains("cmakefiles") || parts.contains("debug") || parts.contains("release");
}
}
void stop(const std::atomic_bool& cancelled) { if(cancelled.load()) throw std::runtime_error("Cancelled. Unprocessed files were left in place."); }
QString rule(const QString& path,qint64 modified,bool installers,bool builds) {
    const QFileInfo info(path); const auto ext=info.suffix().toCaseFolded(),name=info.fileName().toCaseFolded();
    if(builds && buildPath(path) && (QStringList{"obj","pch","ilk","tlog"}.contains(ext) || name=="cmakecache.txt")) return "build";
    if(installers && modified<QDateTime::currentDateTimeUtc().addDays(-180).toMSecsSinceEpoch() &&
       (ext=="msi" || (ext=="exe" && (name.contains("setup") || name.contains("installer"))))) return "installer";
    return {};
}
void guard(const QString& path,const QStringList& protectedFolders,bool generated) {
    if(!QRegularExpression("^[A-Za-z]:/[^:]*$").match(path).hasMatch()) throw std::runtime_error("Cleanup supports ordinary local drive paths only");
    for(const auto& part:path.mid(3).split('/')) if(part.endsWith('.') || part.endsWith(' ')) throw std::runtime_error("Ambiguous path excluded");
    for(const auto& folder:protectedFolders) if(withinPath(path,folder)) throw std::runtime_error("Application and index folders are protected");
    for(const auto* variable:{"SystemRoot","ProgramFiles","ProgramFiles(x86)","ProgramData"}) { auto p=qEnvironmentVariable(variable); if(!p.isEmpty() && withinPath(path,p)) throw std::runtime_error("System/application folder is protected"); }
    QDir dir(QFileInfo(path).absolutePath());
    for(;;) {
        const auto n=dir.dirName().toCaseFolded();
        if(QStringList{".git",".hg",".svn",".tools","node_modules",".venv","$recycle.bin","system volume information"}.contains(n)) throw std::runtime_error("Protected folder excluded");
        if(!generated && (n=="build" || dir.exists(".git") || dir.exists(".hg") || dir.exists(".svn") || dir.exists("CMakeLists.txt"))) throw std::runtime_error("Repository/build files require an explicit supported build-artifact rule");
        if(!dir.cdUp()) break;
    }
}
std::vector<Held> parents(const QString& path) {
    QDir d(QFileInfo(path).absolutePath()); QStringList paths;
    for(;;) { paths.prepend(d.absolutePath()); if(!d.cdUp()) break; }
    std::vector<Held> result; for(const auto& p:paths) result.push_back(handle(p,FILE_READ_ATTRIBUTES,FILE_SHARE_READ,true)); return result;
}
Held open(const QString& path,bool shellDelete) {
    auto result=handle(path,GENERIC_READ,FILE_SHARE_READ|(shellDelete?FILE_SHARE_DELETE:0),false);
    WIN32_FIND_STREAM_DATA data{}; auto find=FindFirstStreamW(native(path).c_str(),FindStreamInfoStandard,&data,0);
    if(find==INVALID_HANDLE_VALUE) { if(GetLastError()==ERROR_HANDLE_EOF) return result; throw std::runtime_error("Cannot verify file streams"); }
    bool extra=false; do { if(QString::fromWCharArray(data.cStreamName)!="::$DATA") extra=true; } while(FindNextStreamW(find,&data));
    auto error=GetLastError(); FindClose(find);
    if(extra || error!=ERROR_HANDLE_EOF) throw std::runtime_error("Files with alternate streams are excluded from cleanup"); return result;
}
QJsonObject snapshot(HANDLE file,const std::atomic_bool& cancelled) {
    BY_HANDLE_FILE_INFORMATION i{}; check(GetFileInformationByHandle(file,&i),"Cannot identify file");
    FILE_STANDARD_INFO allocation{}; check(GetFileInformationByHandleEx(file,FileStandardInfo,&allocation,sizeof(allocation)),"Cannot measure allocated bytes");
    rewind(file); QCryptographicHash hash(QCryptographicHash::Sha256); QByteArray b(1024*1024,Qt::Uninitialized);
    for(;;) { stop(cancelled); auto n=read(file,b); if(!n) break; hash.addData(QByteArrayView(b.constData(),n)); }
    return {{"volume",QString::number(i.dwVolumeSerialNumber)},{"file",QString::number((quint64(i.nFileIndexHigh)<<32)|i.nFileIndexLow)},
        {"created",ticks(i.ftCreationTime)},{"modified",ticks(i.ftLastWriteTime)},{"size",QString::number((quint64(i.nFileSizeHigh)<<32)|i.nFileSizeLow)},
        {"allocated",QString::number(allocation.AllocationSize.QuadPart)},{"sha256",QString::fromLatin1(hash.result().toHex())}};
}
bool sameFile(const QJsonObject& a,const QJsonObject& b) { return a["volume"]==b["volume"] && a["file"]==b["file"]; }
void expect(HANDLE file,const QJsonObject& e,const std::atomic_bool& cancelled) {
    const auto a=snapshot(file,cancelled);
    if(!sameFile(a,e) || a["created"]!=e["created"] || a["modified"]!=e["modified"] || a["size"]!=e["size"] || a["sha256"]!=e["sha256"]) throw std::runtime_error("File changed or was replaced since review. Scan again.");
}
bool equal(HANDLE a,HANDLE b,const std::atomic_bool& cancelled) {
    rewind(a); rewind(b); QByteArray x(1024*1024,Qt::Uninitialized),y(x.size(),Qt::Uninitialized);
    for(;;) { stop(cancelled); const auto nx=read(a,x),ny=read(b,y); if(nx!=ny || memcmp(x.data(),y.data(),nx)) return false; if(!nx) return true; }
}
}
