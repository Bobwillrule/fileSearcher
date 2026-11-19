#include "cleanup/cleanup_service.h"
#include "cleanup/file_checks.h"
#include "cleanup/recycle_file.h"
#include "cleanup/cleanup_controller.h"
#include "storage/metadata_store.h"
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QThread>
#include <QProcess>
#include <shobjidl.h>
using namespace filesearcher;
namespace {
void save(const QString& path,const QByteArray& bytes) { QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); REQUIRE(f.open(QIODevice::WriteOnly)); REQUIRE(f.write(bytes)==bytes.size()); }
QByteArray read(const QString& path) { QFile f(path); if(!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
void alterKeepingTime(const QString& path) {
    HANDLE h=CreateFileW(path.toStdWString().c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr); REQUIRE(h!=INVALID_HANDLE_VALUE);
    FILETIME t{}; REQUIRE(GetFileTime(h,nullptr,nullptr,&t)); DWORD n=0; REQUIRE(WriteFile(h,"x",1,&n,nullptr)); REQUIRE(SetFileTime(h,nullptr,nullptr,&t)); CloseHandle(h);
}
struct Fixture {
    QTemporaryDir temp;
    QString root=temp.path()+"/documents",data=temp.path()+"/index",metadata=data+"/metadata.sqlite";
    std::atomic_bool cancelled=false;
    std::unique_ptr<MetadataStore> index;
    std::unique_ptr<OrganizationStore> store;
    std::unique_ptr<CleanupService> service;
    qint64 rootId;
    Fixture() { REQUIRE(QDir().mkpath(root)); REQUIRE(QDir().mkpath(data)); index=std::make_unique<MetadataStore>(metadata,true); rootId=index->addRoot(root,{}); store=std::make_unique<OrganizationStore>(data+"/organization.sqlite"); service=std::make_unique<CleanupService>(*store,QStringList{data},cancelled); }
    QString add(const QString& name,const QByteArray& bytes,qint64 ageDays=0) {
        auto path=root+'/'+name; save(path,bytes);
        if(ageDays) { QFile file(path); REQUIRE(file.open(QIODevice::ReadWrite)); REQUIRE(file.setFileTime(QDateTime::currentDateTimeUtc().addDays(-ageDays),QFileDevice::FileModificationTime)); }
        record(path); return path;
    }
    void record(const QString& path) { QFileInfo f(path); index->beginScan(); index->record(rootId,"test",{path,f.fileName(),f.suffix(),f.size(),f.lastModified().toMSecsSinceEpoch()}); index->finishScan(rootId,"test",false); }
    QJsonObject candidate() { auto r=service->scan(metadata,rootId,false,false); REQUIRE(r.candidates.size()==1); return QJsonObject::fromVariantMap(r.candidates.front().toMap()); }
    void pair() { add("keep.txt","synthetic exact copy"); add("other.txt","synthetic exact copy"); }
};
void wait(const std::function<bool()>& ready) { QElapsedTimer t; t.start(); while(t.elapsed()<15000) { QCoreApplication::processEvents(); if(ready()) return; QThread::msleep(2); } REQUIRE(ready()); }
}
TEST_CASE("Exact candidates require matching bytes and are scoped to one indexed folder") {
    Fixture f; f.pair(); f.add("different.txt","synthetic wrong copy");
    const auto elsewhere=f.temp.path()+"/outside"; QDir().mkpath(elsewhere); save(elsewhere+"/copy.txt","synthetic exact copy"); f.index->addRoot(elsewhere,{});
    const auto c=f.candidate(); CHECK(c["kind"]=="exact"); CHECK(c["keep"].toString().endsWith("keep.txt")); CHECK(c["path"].toString().endsWith("other.txt")); CHECK(c["allocated"].toString().toLongLong()>0);
    REQUIRE_THROWS(f.service->scan(f.metadata,0,false,false));
    auto a=cleanup::open(f.root+"/keep.txt"),b=cleanup::open(f.root+"/different.txt"); CHECK_FALSE(cleanup::equal(a->value,b->value,f.cancelled));
}
TEST_CASE("Hard-link names are excluded and never counted as separately reclaimable copies") {
    Fixture f; auto original=f.add("a.txt","hardlink fixture"); auto link=f.root+"/b.txt";
    REQUIRE(CreateHardLinkW(link.toStdWString().c_str(),original.toStdWString().c_str(),nullptr)); f.record(link);
    auto scan=f.service->scan(f.metadata,f.rootId,false,false); CHECK(scan.candidates.isEmpty()); CHECK(scan.skipped==2);
}
TEST_CASE("Stale same-size content and replaced retained copies stop cleanup") {
    Fixture f; f.pair(); auto c=f.candidate();
    SECTION("changed candidate before review") { alterKeepingTime(c["path"].toString()); REQUIRE_THROWS(f.service->prepare(c)); }
    SECTION("changed retained copy after review") { auto op=f.service->prepare(c); alterKeepingTime(c["keep"].toString()); REQUIRE_THROWS(f.service->execute(op["id"].toString())); CHECK(f.service->operation(op["id"].toString())["state"]=="failed"); }
    SECTION("changed candidate after review") { auto op=f.service->prepare(c); alterKeepingTime(c["path"].toString()); REQUIRE_THROWS(f.service->execute(op["id"].toString())); }
    CHECK(QFileInfo::exists(c["path"].toString())); CHECK(QFileInfo::exists(c["keep"].toString()));
}
TEST_CASE("Unstarted reviews can be reversed or cancelled without changing files") {
    Fixture f; f.pair(); const auto c=f.candidate(); auto op=f.service->prepare(c,true);
    CHECK(op["keep"]==c["path"]); CHECK(op["path"]==c["keep"]); f.service->cancel(op["id"].toString());
    REQUIRE_THROWS(f.service->execute(op["id"].toString())); CHECK(read(c["path"].toString())==read(c["keep"].toString()));
}
TEST_CASE("Cancellation and injected failure preserve the retained copy") {
    Fixture f; f.pair(); auto op=f.service->prepare(f.candidate());
    SECTION("cancelled before work") { f.cancelled=true; }
    SECTION("retained file stays locked against writes and removal") {
        f.service->checkpoint=[&](const QString& stage) { if(stage=="validated") { QFile file(op["keep"].toString()); CHECK_FALSE(file.open(QIODevice::WriteOnly)); CHECK_FALSE(QFile::remove(op["keep"].toString())); throw std::runtime_error("Injected pre-recycle failure"); } };
    }
    SECTION("journal saved but shell not called") { f.service->checkpoint=[](const QString& stage){if(stage=="recycling") throw std::runtime_error("Simulated interruption");}; }
    REQUIRE_THROWS(f.service->execute(op["id"].toString())); CHECK(read(op["path"].toString())=="synthetic exact copy"); CHECK(read(op["keep"].toString())=="synthetic exact copy");
    f.service->recover(); REQUIRE_THROWS(f.service->execute(op["id"].toString()));
}
TEST_CASE("Opt-in cleanup rules require recognized artifact names and context") {
    Fixture f; f.add("legacy-setup.exe","installer",200); f.add("ordinary.exe","old program",200); f.add("new-setup.exe","new installer"); f.add("readme.txt","old text",300);
    f.add("build/file.obj","generated object"); f.add("outside.obj","not build context"); f.add("build/source.cpp","user source");
    CHECK(f.service->scan(f.metadata,f.rootId,false,false).candidates.isEmpty());
    const auto scan=f.service->scan(f.metadata,f.rootId,true,true); REQUIRE(scan.candidates.size()==2);
    for(const auto& row:scan.candidates) { auto c=QJsonObject::fromVariantMap(row.toMap()); CHECK(c["keep"].toString().isEmpty()); auto op=f.service->prepare(c); f.service->cancel(op["id"].toString()); }
    CHECK(cleanup::rule(f.root+"/ordinary.exe",0,true,false).isEmpty());
}
TEST_CASE("Protected repository paths and alternate data streams are excluded") {
    Fixture f; f.pair();
    SECTION("repository") { save(f.root+"/CMakeLists.txt","project(sample)"); }
    SECTION("alternate streams") { save(f.root+"/keep.txt:metadata","different metadata"); }
    CHECK(f.service->scan(f.metadata,f.rootId,false,false).candidates.isEmpty());
}
TEST_CASE("Interrupted recycling is observational and cleanup history survives index clearing") {
    Fixture f; f.pair(); auto op=f.service->prepare(f.candidate()); op["state"]="recycling";
    f.store->query("UPDATE cleanup_operations SET body=? WHERE id=?",{QString::fromUtf8(QJsonDocument(op).toJson(QJsonDocument::Compact)),op["id"].toString()});
    f.service->recover(); CHECK(f.service->operation(op["id"].toString())["state"]=="needs_review"); f.index->clearSavedIndex();
    CHECK(f.service->history().size()==1); CHECK(read(op["path"].toString())==read(op["keep"].toString()));
}
TEST_CASE("Permanent-delete callbacks and cancelled recycling are refused") {
    CHECK_FALSE(recyclingAllowed(0)); CHECK_FALSE(recyclingAllowed(TSF_USE_FULL_ACCESS)); CHECK(recyclingAllowed(TSF_DELETE_RECYCLE_IF_POSSIBLE));
    Fixture f; auto path=f.add("cancel-recycle.txt","synthetic cancel fixture");
    std::atomic_bool cancelled=false; bool called=false;
    REQUIRE_THROWS(recycleFile(path,[&]{called=true; throw std::runtime_error("User cancelled at the last checkpoint");},cancelled));
    CHECK(called); CHECK(read(path)=="synthetic cancel fixture");
}
TEST_CASE("Real Windows recycling confirms a bin location and retains the verified copy") {
    Fixture f; f.add(QString::fromUtf8("保留.txt"),"FileSearcher synthetic recycle integration fixture"); f.add(QString::fromUtf8("副本.txt"),"FileSearcher synthetic recycle integration fixture");
    const auto op=f.service->prepare(f.candidate()); QJsonObject outcome;
    try { outcome=f.service->execute(op["id"].toString()); } catch(const std::exception& e) { FAIL(e.what()); }
    CHECK(outcome["state"]=="recycled"); CHECK_FALSE(QFileInfo::exists(outcome["path"].toString()));
    CHECK(read(outcome["keep"].toString())=="FileSearcher synthetic recycle integration fixture");
    CHECK(read(outcome["recycledPath"].toString())=="FileSearcher synthetic recycle integration fixture");
    REQUIRE_THROWS(f.service->execute(op["id"].toString()));
    // Only this tiny synthetic fixture is left in Windows Recycle Bin. Never empty the user's bin.
}
TEST_CASE("Cleanup controller requires a separate approval and supports cancelling review") {
    Fixture f; f.pair(); CleanupController c(f.data); wait([&]{return !c.busy();});
    c.scan(f.rootId,false,false); wait([&]{return !c.busy();}); REQUIRE(c.candidates().size()==1);
    c.prepare(c.candidates().front().toMap()["token"].toString()); wait([&]{return !c.busy();}); REQUIRE(!c.plan().isEmpty());
    CHECK(QDir(f.root).entryList(QDir::Files).size()==2); c.approve("unreviewed-id"); CHECK_FALSE(c.busy());
    c.dismiss(c.plan()["id"].toString()); wait([&]{return !c.busy();}); CHECK(c.plan().isEmpty()); CHECK(QDir(f.root).entryList(QDir::Files).size()==2);
}
TEST_CASE("Abrupt termination before and after recycling never triggers automatic deletion on restart") {
    for(const auto& stage:{QString("recycling"),QString("recycled")}) {
        Fixture f; f.pair(); const auto op=f.service->prepare(f.candidate());
        QProcess child; child.start(QCoreApplication::applicationFilePath(),{"--crash-cleanup",f.data+"/organization.sqlite",op["id"].toString(),stage});
        REQUIRE(child.waitForFinished(20000)); REQUIRE(child.exitCode()==44);
        CHECK(read(op["keep"].toString())=="synthetic exact copy");
        const bool sourcePresent=QFileInfo::exists(op["path"].toString()); CHECK(sourcePresent==(stage=="recycling"));
        f.service->recover(); CHECK(f.service->operation(op["id"].toString())["state"]=="needs_review");
        CHECK(QFileInfo::exists(op["path"].toString())==sourcePresent); REQUIRE_THROWS(f.service->execute(op["id"].toString()));
    }
}
TEST_CASE("Junction paths are refused before duplicate hashing") {
    Fixture f; f.pair(); auto link=f.temp.path()+"/linked";
    QProcess command; command.start("cmd.exe",{"/d","/c","mklink","/J",QDir::toNativeSeparators(link),QDir::toNativeSeparators(f.root)});
    REQUIRE(command.waitForFinished()); REQUIRE(command.exitCode()==0);
    struct RemoveLink { QString path; ~RemoveLink(){RemoveDirectoryW(path.toStdWString().c_str());} } cleanupLink{link};
    REQUIRE_THROWS(cleanup::parents(link+"/keep.txt"));
}
TEST_CASE("Possible versions remain discovery-only even when AI is off") {
    Fixture f; f.add("resume-v1.txt","earlier revision"); f.add("resume-v2.txt","newer changed revision");
    CleanupController c(f.data); c.setAiEnabled(false); wait([&]{return !c.busy();});
    SearchOptions options; options.query="resume-v1"; const auto found=f.index->search(options); REQUIRE(found.rows.size()==1);
    c.inspect(found.rows.front().toMap()["fileId"].toLongLong()); wait([&]{return !c.busy();}); REQUIRE(c.related().size()==1);
    CHECK(c.related().front().toMap()["kind"]=="Possible version"); CHECK(c.plan().isEmpty()); CHECK(c.candidates().isEmpty());
}
TEST_CASE("Read-only copies are excluded and zero-byte duplicates claim no disk allocation") {
    Fixture f; auto a=f.add("a.txt",{}); f.add("b.txt",{});
    const auto c=f.candidate(); CHECK(c["allocated"].toString().toLongLong()==0);
    REQUIRE(SetFileAttributesW(a.toStdWString().c_str(),FILE_ATTRIBUTE_READONLY));
    struct ResetAttributes { QString path; ~ResetAttributes(){SetFileAttributesW(path.toStdWString().c_str(),FILE_ATTRIBUTE_NORMAL);} } reset{a};
    CHECK(f.service->scan(f.metadata,f.rootId,false,false).candidates.isEmpty());
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); const auto args=app.arguments();
    if(args.size()==5 && args[1]=="--crash-cleanup") {
        std::atomic_bool cancelled=false; OrganizationStore store(args[2]); CleanupService service(store,{},cancelled);
        service.checkpoint=[&](const QString& stage){if(stage==args[4]) TerminateProcess(GetCurrentProcess(),44);};
        try { service.execute(args[3]); } catch(...) { return 3; } return 4;
    }
    return Catch::Session().run(argc,argv);
}
