#include "organization/move_service.h"
#include "storage/metadata_store.h"
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>
#include <memory>
#include <vector>
#include <stdexcept>
#define NOMINMAX
#include <windows.h>

namespace filesearcher {
namespace {
void check(bool ok,const char* what) { if(!ok) throw std::runtime_error(QString("%1 (Windows error %2)").arg(what).arg(GetLastError()).toStdString()); }
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h):value(h) {}
    ~Handle() { if(value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&)=delete;
};
using Held=std::unique_ptr<Handle>;
std::wstring native(const QString& path) { return ("\\\\?\\"+QDir::toNativeSeparators(path)).toStdWString(); }
Held open(const QString& path,DWORD access,bool directory=false,DWORD creation=OPEN_EXISTING) {
    const auto name=native(path);
    auto handle=std::make_unique<Handle>(CreateFileW(name.c_str(),access,FILE_SHARE_READ,nullptr,creation,
        FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:FILE_FLAG_SEQUENTIAL_SCAN),nullptr));
    check(handle->value!=INVALID_HANDLE_VALUE,"Cannot lock the file or folder; close other applications and retry");
    BY_HANDLE_FILE_INFORMATION info{}; check(GetFileInformationByHandle(handle->value,&info),"Cannot identify file");
    constexpr DWORD excluded=FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_OFFLINE|FILE_ATTRIBUTE_RECALL_ON_OPEN|FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS|FILE_ATTRIBUTE_ENCRYPTED;
    if((info.dwFileAttributes&excluded) || bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=directory)
        throw std::runtime_error("Only ordinary local files and folders are supported; links, cloud placeholders and encrypted files are excluded");
    if(!directory && (info.nNumberOfLinks!=1 || (info.dwFileAttributes&FILE_ATTRIBUTE_READONLY)))
        throw std::runtime_error("Read-only files and files with hard links cannot be moved here");
    std::vector<wchar_t> resolved(32768);
    const auto length=GetFinalPathNameByHandleW(handle->value,resolved.data(),static_cast<DWORD>(resolved.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    check(length>0 && length<resolved.size(),"Cannot resolve file identity");
    if(QString::compare(QString::fromWCharArray(resolved.data(),length),QString::fromStdWString(name),Qt::CaseInsensitive))
        throw std::runtime_error("The path resolves through a different location; no move was performed");
    return handle;
}
std::vector<Held> pinParents(const QString& directory) {
    QStringList paths; QDir current(directory);
    for(;;) { paths.prepend(current.absolutePath()); if(!current.cdUp()) break; }
    std::vector<Held> handles;
    for(const auto& path:paths) handles.push_back(open(path,FILE_READ_ATTRIBUTES,true));
    return handles;
}
QString ticks(FILETIME value) { return QString::number((quint64(value.dwHighDateTime)<<32)|value.dwLowDateTime); }
QJsonObject identity(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION i{}; check(GetFileInformationByHandle(handle,&i),"Cannot read file identity");
    return {{"volume",QString::number(i.dwVolumeSerialNumber)},
        {"file",QString::number((quint64(i.nFileIndexHigh)<<32)|i.nFileIndexLow)},
        {"created",ticks(i.ftCreationTime)},{"modified",ticks(i.ftLastWriteTime)},
        {"size",QString::number((quint64(i.nFileSizeHigh)<<32)|i.nFileSizeLow)}};
}
bool sameIdentity(const QJsonObject& a,const QJsonObject& b) {
    return !a.isEmpty() && a["volume"]==b["volume"] && a["file"]==b["file"] && a["created"]==b["created"];
}
QString hash(HANDLE handle,const std::atomic_bool* cancelled) {
    LARGE_INTEGER zero{}; check(SetFilePointerEx(handle,zero,nullptr,FILE_BEGIN),"Cannot read file");
    QCryptographicHash hash(QCryptographicHash::Sha256); QByteArray bytes(1024*1024,Qt::Uninitialized);
    for(;;) {
        if(cancelled && cancelled->load()) throw std::runtime_error("Operation interrupted; review its recorded state before continuing");
        DWORD count=0; check(ReadFile(handle,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr),"Cannot read file");
        if(!count) break; hash.addData(QByteArrayView(bytes.constData(),count));
    }
    return QString::fromLatin1(hash.result().toHex());
}
QJsonObject snapshot(HANDLE handle,const std::atomic_bool* cancelled) { auto result=identity(handle); result["sha256"]=hash(handle,cancelled); return result; }
void expect(HANDLE handle,const QJsonObject& expected,const std::atomic_bool* cancelled) {
    const auto actual=snapshot(handle,cancelled);
    if(!sameIdentity(actual,expected) || actual["size"]!=expected["size"] || actual["modified"]!=expected["modified"] || actual["sha256"]!=expected["sha256"])
        throw std::runtime_error("The file has changed or was replaced since review. No destructive action was taken");
}
void absent(const QString& path) {
    const auto attrs=GetFileAttributesW(native(path).c_str());
    if(attrs!=INVALID_FILE_ATTRIBUTES) throw std::runtime_error("A file or folder already exists at the destination; nothing will be overwritten");
    const auto error=GetLastError(); if(error!=ERROR_FILE_NOT_FOUND) throw std::runtime_error("The destination cannot be verified as empty");
}
void rename(HANDLE handle,const QString& destination) {
    const auto name=native(destination); std::vector<unsigned char> bytes(sizeof(FILE_RENAME_INFO)+name.size()*sizeof(wchar_t));
    auto* info=reinterpret_cast<FILE_RENAME_INFO*>(bytes.data()); info->ReplaceIfExists=FALSE; info->RootDirectory=nullptr;
    info->FileNameLength=static_cast<DWORD>(name.size()*sizeof(wchar_t)); memcpy(info->FileName,name.c_str(),info->FileNameLength);
    check(SetFileInformationByHandle(handle,FileRenameInfo,info,static_cast<DWORD>(bytes.size())),"Cannot move file; destination may be occupied");
}
void remove(HANDLE handle) {
    FILE_DISPOSITION_INFO info{TRUE}; check(SetFileInformationByHandle(handle,FileDispositionInfo,&info,sizeof(info)),"Copy is safe, but the original could not be removed");
}
void requireSingleStream(const QString& path) {
    WIN32_FIND_STREAM_DATA stream{}; const auto h=FindFirstStreamW(native(path).c_str(),FindStreamInfoStandard,&stream,0);
    if(h==INVALID_HANDLE_VALUE) { if(GetLastError()==ERROR_HANDLE_EOF) return; throw std::runtime_error("Cannot inspect alternate data streams"); }
    bool extra=false; do { if(QString::fromWCharArray(stream.cStreamName)!="::$DATA") extra=true; } while(FindNextStreamW(h,&stream));
    const auto error=GetLastError(); FindClose(h);
    if(error!=ERROR_HANDLE_EOF || extra) throw std::runtime_error("Cross-volume copying of alternate data streams is not supported. Use Explorer; the original is unchanged");
}
}
MoveService::MoveService(OrganizationStore& store,QStringList protectedFolders,const std::atomic_bool* cancelled):store_(store),protected_(std::move(protectedFolders)),cancelled_(cancelled) {
    for(const auto& variable:{"SystemRoot","ProgramFiles","ProgramFiles(x86)"}) {
        const auto path=qEnvironmentVariable(variable); if(!path.isEmpty()) protected_<<path;
    }
}
void MoveService::tick(const QString& stage) const {
    if(checkpoint) checkpoint(stage);
    if(cancelled_ && cancelled_->load()) throw std::runtime_error("Operation interrupted; its journal preserves recovery information");
}
void MoveService::guard(const QString& path) const {
    static const QRegularExpression local("^[A-Za-z]:/[^:]*$");
    if(!local.match(path).hasMatch()) throw std::runtime_error("Choose an absolute local drive path; network/device/stream paths are not supported");
    for(const auto& segment:path.mid(3).split('/')) if(segment.endsWith(' ') || segment.endsWith('.')) throw std::runtime_error("Paths ending in spaces or dots are not supported");
    for(const auto& folder:protected_) if(withinPath(path,folder)) throw std::runtime_error("Application, system and index folders are protected; use virtual collections instead");
    QDir directory(QFileInfo(path).isDir()?path:QFileInfo(path).absolutePath());
    for(;;) {
        const auto name=directory.dirName().toCaseFolded();
        if(QStringList{"build",".tools","node_modules",".venv",".git",".hg",".svn"}.contains(name) ||
           directory.exists(".git") || directory.exists(".hg") || directory.exists(".svn") || directory.exists("CMakeLists.txt"))
            throw std::runtime_error("Repository and build folders are protected; use virtual collections instead");
        if(!directory.cdUp()) break;
    }
}
QJsonObject MoveService::prepare(const QString& sourcePath,const QString& folderPath) {
    if(!QDir::isAbsolutePath(sourcePath) || !QDir::isAbsolutePath(folderPath)) throw std::runtime_error("Choose absolute source and destination paths");
    const auto source=normalizedPath(sourcePath),folder=normalizedPath(folderPath),destination=folder+'/'+QFileInfo(source).fileName();
    guard(source); guard(folder); if(pathKey(source)==pathKey(destination)) throw std::runtime_error("The file is already in that folder");
    auto sourceParents=pinParents(QFileInfo(source).absolutePath()),targetParents=pinParents(folder);
    auto input=open(source,GENERIC_READ); absent(destination);
    const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject operation{{"id",id},{"state","planned"},{"source",source},{"destination",destination},
        {"temporary",folder+"/.filesearcher-"+id+".partial"},{"sourceSnapshot",snapshot(input->value,cancelled_)},
        {"sourceParentSnapshot",identity(sourceParents.back()->value)},
        {"parentSnapshot",identity(targetParents.back()->value)},{"createdAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"undoOf",""},{"message","Reviewed plan only. Files have not been moved."}};
    store_.putOperation(operation); tick("planned"); return operation;
}
void MoveService::cancel(const QString& id) {
    auto op=store_.operation(id); if(op["state"]!="planned") throw std::runtime_error("Only an unstarted plan can be cancelled");
    op["state"]="cancelled"; op["message"]="Plan cancelled; files unchanged."; store_.putOperation(op);
}
QJsonObject MoveService::prepareUndo(const QString& id) {
    const auto original=store_.operation(id);
    if(original["state"]!="completed" || !original["undoOf"].toString().isEmpty()) throw std::runtime_error("Only a completed forward move can be undone");
    auto originalParents=pinParents(QFileInfo(original["source"].toString()).absolutePath());
    if(!sameIdentity(identity(originalParents.back()->value),original["sourceParentSnapshot"].toObject())) throw std::runtime_error("The original folder changed after the move; undo was refused");
    auto parents=pinParents(QFileInfo(original["destination"].toString()).absolutePath());
    auto source=open(original["destination"].toString(),GENERIC_READ); expect(source->value,original["destinationSnapshot"].toObject(),cancelled_);
    source.reset(); parents.clear();
    auto op=prepare(original["destination"].toString(),QFileInfo(original["source"].toString()).absolutePath());
    if(op["sourceSnapshot"]!=original["destinationSnapshot"]) { cancel(op["id"].toString()); throw std::runtime_error("Moved file changed while preparing undo"); }
    op["undoOf"]=id; store_.putOperation(op); return op;
}
void MoveService::execute(const QString& id) {
    auto op=store_.operation(id); const auto state=op["state"].toString();
    if(state!="planned" && state!="copy_ready") throw std::runtime_error("This move requires review; it cannot be automatically retried");
    if(!op["undoOf"].toString().isEmpty() && store_.operation(op["undoOf"].toString())["state"]!="completed") throw std::runtime_error("The original move is no longer eligible for undo");
    const auto source=op["source"].toString(),destination=op["destination"].toString(),folder=QFileInfo(destination).absolutePath();
    guard(source); guard(destination);
    auto sourceParents=pinParents(QFileInfo(source).absolutePath()),targetParents=pinParents(folder);
    if(!sameIdentity(identity(sourceParents.back()->value),op["sourceParentSnapshot"].toObject())) throw std::runtime_error("Source folder was replaced after review");
    if(!sameIdentity(identity(targetParents.back()->value),op["parentSnapshot"].toObject())) throw std::runtime_error("Destination folder was replaced after review");
    auto input=open(source,GENERIC_READ|DELETE); expect(input->value,op["sourceSnapshot"].toObject(),cancelled_);
    if(state=="copy_ready") {
        auto output=open(destination,GENERIC_READ); expect(output->value,op["destinationSnapshot"].toObject(),cancelled_);
        op["state"]="removing"; store_.putOperation(op); tick("removing"); remove(input->value); input.reset(); tick("source_removed"); store_.complete(op); return;
    }
    absent(destination);
    const bool copy=forceCopyForTesting || identity(input->value)["volume"]!=identity(targetParents.back()->value)["volume"];
    if(copy) requireSingleStream(source);
    op["state"]="executing"; op["destinationSnapshot"]=copy?QJsonObject{}:op["sourceSnapshot"].toObject(); store_.putOperation(op); tick("executing");
    if(!copy) {
        rename(input->value,destination); tick("renamed"); store_.complete(op); return;
    }
    const auto temp=op["temporary"].toString();
    auto output=open(temp,GENERIC_READ|GENERIC_WRITE|DELETE,false,CREATE_NEW); tick("temporary_created");
    auto expected=identity(output->value); const auto original=op["sourceSnapshot"].toObject();
    expected["size"]=original["size"]; expected["modified"]=original["modified"]; expected["sha256"]=original["sha256"];
    op["destinationSnapshot"]=expected; op["state"]="copying"; store_.putOperation(op); tick("copying");
    LARGE_INTEGER zero{}; check(SetFilePointerEx(input->value,zero,nullptr,FILE_BEGIN),"Cannot read source");
    QByteArray buffer(1024*1024,Qt::Uninitialized);
    for(;;) {
        DWORD count=0; check(ReadFile(input->value,buffer.data(),static_cast<DWORD>(buffer.size()),&count,nullptr),"Cannot copy source"); if(!count) break;
        DWORD written=0; check(WriteFile(output->value,buffer.constData(),count,&written,nullptr) && written==count,"Cannot write temporary copy"); tick("partial_copy");
    }
    FILETIME created{},accessed{},modified{}; check(GetFileTime(input->value,&created,&accessed,&modified),"Cannot read file times");
    check(SetFileTime(output->value,nullptr,&accessed,&modified),"Cannot preserve last-write time");
    check(FlushFileBuffers(output->value),"Cannot flush verified copy");
    expect(output->value,expected,cancelled_); op["state"]="publishing"; store_.putOperation(op); tick("publishing");
    absent(destination); rename(output->value,destination); check(FlushFileBuffers(output->value),"Cannot flush the published destination"); tick("published");
    op["state"]="removing"; store_.putOperation(op); tick("removing");
    expect(input->value,original,cancelled_); remove(input->value); input.reset(); tick("source_removed"); store_.complete(op);
}
void MoveService::recover() {
    // Scan all unfinished records, including those older than the UI's 100-row history.
    auto q=store_.query("SELECT body FROM moves"); QList<QJsonObject> pending;
    while(q.next()) pending<<QJsonDocument::fromJson(q.value(0).toByteArray()).object(); q.finish();
    for(auto op:pending) {
        const auto state=op["state"].toString();
        if(state=="planned" || state=="completed" || state=="cancelled" || state=="undone") continue;
        try {
            const auto source=op["source"].toString(),destination=op["destination"].toString(); guard(source); guard(destination);
            auto parents=pinParents(QFileInfo(destination).absolutePath());
            if(!sameIdentity(identity(parents.back()->value),op["parentSnapshot"].toObject())) throw std::runtime_error("Destination folder changed");
            auto output=open(destination,GENERIC_READ); expect(output->value,op["destinationSnapshot"].toObject(),cancelled_);
            const auto attrs=GetFileAttributesW(native(source).c_str()); const auto error=GetLastError();
            if(attrs==INVALID_FILE_ATTRIBUTES && error==ERROR_FILE_NOT_FOUND) { store_.complete(op); continue; }
            auto sourceParents=pinParents(QFileInfo(source).absolutePath()); auto input=open(source,GENERIC_READ);
            expect(input->value,op["sourceSnapshot"].toObject(),cancelled_);
            op["state"]="copy_ready"; op["message"]="Both verified copies exist. Review and Finish move to remove the original, or keep both. Nothing was deleted during recovery.";
        } catch(const std::exception&) {
            op["state"]="needs_review"; op["message"]="Interrupted or changed files require manual review. No files were altered during recovery. Inspect source, destination and the recorded temporary path; partial copies are deliberately retained.";
        }
        store_.putOperation(op);
    }
}
}
