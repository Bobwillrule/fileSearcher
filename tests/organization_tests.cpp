#include "organization/move_service.h"
#include "organization/file_intelligence.h"
#include "storage/metadata_store.h"
#include "storage/embedding_store.h"
#include "organization/organization_controller.h"
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QElapsedTimer>
#include <QThread>
#include <QProcess>
#include <stdexcept>
#define NOMINMAX
#include <windows.h>
using namespace filesearcher;
namespace {
void save(const QString& path,const QByteArray& bytes) { QFile f(path); REQUIRE(f.open(QIODevice::WriteOnly)); REQUIRE(f.write(bytes)==bytes.size()); }
QByteArray read(const QString& path) { QFile f(path); if(!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
struct Fixture {
    QTemporaryDir temp;
    QString data=temp.path()+"/index",root=temp.path()+"/documents",target=temp.path()+"/organized",database=data+"/metadata.sqlite",journal=data+"/organization.sqlite";
    std::unique_ptr<MetadataStore> metadata;
    Fixture() { REQUIRE(QDir().mkpath(data)); REQUIRE(QDir().mkpath(root)); REQUIRE(QDir().mkpath(target)); metadata=std::make_unique<MetadataStore>(database,true); metadata->addRoot(root,{}); }
    qint64 add(const QString& name,const QByteArray& text,int axis=0) {
        const auto path=root+'/'+name; save(path,text); QFileInfo info(path);
        const auto rootId=metadata->roots().front().id;
        metadata->beginScan(); metadata->record(rootId,"test",{path,name,info.suffix(),info.size(),info.lastModified().toMSecsSinceEpoch()}); metadata->finishScan(rootId,"test",false);
        const auto pending=metadata->pendingContent(rootId); REQUIRE(pending.size()==1);
        REQUIRE(metadata->replaceContent(pending.front(),"ok",{{QString::fromUtf8(text),"lines 1-2",20}}));
        if(axis>=0) {
            EmbeddingStore vectors(database);
            for(const auto& passage:vectors.pendingPassages(rootId)) REQUIRE(vectors.prepare(passage,{{0,static_cast<int>(passage.text.size()),20,passage.text}}));
            const auto work=vectors.pending(rootId,8); std::vector<float> vector(384); vector[axis]=1;
            REQUIRE(vectors.commit(work,std::vector<std::vector<float>>(work.size(),vector))==work.size());
        }
        return pending.front().id;
    }
};
void waitFor(const std::function<bool()>& ready) { QElapsedTimer t; t.start(); while(t.elapsed()<15000) { QCoreApplication::processEvents(); if(ready()) return; QThread::msleep(2); } REQUIRE(ready()); }
void changePreservingWriteTime(const QString& path) {
    auto h=CreateFileW(path.toStdWString().c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr); REQUIRE(h!=INVALID_HANDLE_VALUE);
    FILETIME created{},accessed{},modified{}; REQUIRE(GetFileTime(h,&created,&accessed,&modified)); CloseHandle(h);
    auto bytes=read(path); REQUIRE(!bytes.isEmpty()); bytes[0]=bytes[0]=='X'?'Y':'X'; save(path,bytes);
    h=CreateFileW(path.toStdWString().c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr); REQUIRE(h!=INVALID_HANDLE_VALUE);
    REQUIRE(SetFileTime(h,nullptr,nullptr,&modified)); CloseHandle(h);
}
}
TEST_CASE("Related files exclude the selection and explain content and context") {
    Fixture f; const auto source=f.add("diode-notes.txt","Semiconductor diode rectifier electrical current flows forward.");
    const auto relevant=f.add("lab-circuit.txt","A rectifier diode conducts electricity in one direction.");
    const auto unrelated=f.add("cake.txt","Cake sugar flour baking oven temperature.",1);
    std::atomic_bool cancelled{false}; const auto rows=relatedFiles(f.database,f.data+"/vectors",source,cancelled);
    REQUIRE(rows.size()==1); const auto row=rows.front().toMap(); CHECK(row["fileId"].toLongLong()==relevant);
    CHECK(row["fileId"].toLongLong()!=source); CHECK(row["fileId"].toLongLong()!=unrelated);
    CHECK(row["reason"].toString().contains("Similar indexed passages")); CHECK(row["snippet"].toString().contains("rectifier")); CHECK(row["passageId"].toLongLong()>0);
    cancelled=true; CHECK_THROWS(relatedFiles(f.database,f.data+"/vectors",source,cancelled));
}
TEST_CASE("Filename and project evidence works without vectors but same folder alone abstains") {
    Fixture f; const auto source=f.add("circuit-diode-notes.txt","rectifier",-1);
    const auto related=f.add("circuit-diode-lab.txt","current",-1); f.add("recipe.txt","baking",-1);
    std::atomic_bool cancel{false}; auto rows=relatedFiles(f.database,f.data+"/vectors",source,cancel); REQUIRE(rows.size()==1);
    CHECK(rows.front().toMap()["fileId"].toLongLong()==related); CHECK(rows.front().toMap()["reason"].toString().contains("Shared filename"));
}
TEST_CASE("Categories persist, use examples, abstain on ambiguity and remember rejections") {
    Fixture f; const auto file=f.add("lecture.txt","Semiconductor diode rectifier circuits conduct current.");
    f.add("example.txt","Forward current passes through the diode.");
    OrganizationStore store(f.journal); std::atomic_bool cancel{false};
    const auto electrical=store.saveCategory(0,"Electrical","semiconductor diode rectifier",f.target);
    const auto unrelated=store.saveCategory(0,"Finance","invoices income tax",QStringLiteral(""));
    auto suggestions=categorySuggestions(f.database,store,file,cancel); REQUIRE(suggestions.size()==1); CHECK(suggestions.front().toMap()["id"].toLongLong()==electrical);
    store.addExample(unrelated,f.root+"/example.txt"); suggestions=categorySuggestions(f.database,store,file,cancel); REQUIRE(suggestions.size()==1); CHECK(suggestions.front().toMap()["id"].toLongLong()==unrelated);
    CHECK(suggestions.front().toMap()["reason"].toString().contains("example"));
    store.addExample(electrical,f.root+"/example.txt"); CHECK(categorySuggestions(f.database,store,file,cancel).isEmpty());
    store.removeCategory(unrelated); const auto info=organizationFile(f.database,file);
    store.decide(electrical,info["path"].toString(),info["size"].toLongLong(),info["modified"].toLongLong(),"rejected");
    CHECK(categorySuggestions(f.database,store,file,cancel).isEmpty()); CHECK(QFileInfo(info["path"].toString()).isFile());
    store.saveCategory(electrical,"Electrical","Updated diode example",f.target); REQUIRE(categorySuggestions(f.database,store,file,cancel).size()==1);
    store.decide(electrical,info["path"].toString(),info["size"].toLongLong(),info["modified"].toLongLong(),"accepted");
    OrganizationStore reopened(f.journal); CHECK(reopened.members(electrical).size()==1);
    f.metadata->clearSavedIndex(); CHECK(reopened.categories().size()==1); CHECK(reopened.members(electrical).size()==1); CHECK(QFileInfo(info["path"].toString()).isFile());
}
TEST_CASE("Approved Unicode moves preserve bytes, move tags and support checked undo") {
    Fixture f; const QString name=QString::fromUtf8("résumé 東京.txt"); const auto id=f.add(name,"Original contents stay intact.");
    OrganizationStore store(f.journal); const auto category=store.saveCategory(0,"Career","resume",f.target); auto info=organizationFile(f.database,id);
    store.decide(category,info["path"].toString(),info["size"].toLongLong(),info["modified"].toLongLong(),"accepted");
    MoveService moves(store); auto plan=moves.prepare(f.root+'/'+name,f.target); const auto moveId=plan["id"].toString();
    CHECK(QFileInfo(f.root+'/'+name).exists()); CHECK_FALSE(QFileInfo(f.target+'/'+name).exists());
    moves.execute(moveId); CHECK_FALSE(QFileInfo(f.root+'/'+name).exists()); CHECK(read(f.target+'/'+name)=="Original contents stay intact.");
    CHECK(store.operation(moveId)["state"]=="completed"); CHECK(store.members(category).front().toMap()["path"].toString()==f.target+'/'+name);
    auto undo=moves.prepareUndo(moveId); moves.execute(undo["id"].toString());
    CHECK(read(f.root+'/'+name)=="Original contents stay intact."); CHECK_FALSE(QFileInfo(f.target+'/'+name).exists()); CHECK(store.operation(moveId)["state"]=="undone");
    CHECK(store.members(category).front().toMap()["path"].toString()==f.root+'/'+name);
}
TEST_CASE("Move conflicts, changed revisions, replacement directories and cancelled plans preserve files") {
    Fixture f; f.add("one.txt","original"); OrganizationStore store(f.journal); MoveService moves(store);
    auto plan=moves.prepare(f.root+"/one.txt",f.target); save(f.target+"/one.txt","existing"); CHECK_THROWS(moves.execute(plan["id"].toString())); CHECK(read(f.root+"/one.txt")=="original"); CHECK(read(f.target+"/one.txt")=="existing");
    f.add("two.txt","original"); plan=moves.prepare(f.root+"/two.txt",f.target); changePreservingWriteTime(f.root+"/two.txt"); CHECK_THROWS(moves.execute(plan["id"].toString())); CHECK_FALSE(QFileInfo(f.target+"/two.txt").exists());
    f.add("three.txt","original"); plan=moves.prepare(f.root+"/three.txt",f.target); moves.cancel(plan["id"].toString()); CHECK_THROWS(moves.execute(plan["id"].toString())); CHECK(read(f.root+"/three.txt")=="original");
    f.add("four.txt","original"); plan=moves.prepare(f.root+"/four.txt",f.target); REQUIRE(QDir().rename(f.target,f.target+"-old")); REQUIRE(QDir().mkpath(f.target)); CHECK_THROWS(moves.execute(plan["id"].toString())); CHECK(read(f.root+"/four.txt")=="original");
}
TEST_CASE("Undo refuses changed content and a new occupant at the original name") {
    Fixture f; f.add("one.txt","original"); OrganizationStore store(f.journal); MoveService moves(store);
    auto plan=moves.prepare(f.root+"/one.txt",f.target); moves.execute(plan["id"].toString()); changePreservingWriteTime(f.target+"/one.txt"); CHECK_THROWS(moves.prepareUndo(plan["id"].toString()));
    f.add("two.txt","original"); plan=moves.prepare(f.root+"/two.txt",f.target); moves.execute(plan["id"].toString()); save(f.root+"/two.txt","new file"); CHECK_THROWS(moves.prepareUndo(plan["id"].toString())); CHECK(read(f.root+"/two.txt")=="new file"); CHECK(read(f.target+"/two.txt")=="original");
}
TEST_CASE("Junction source and destination paths cannot redirect approved moves") {
    Fixture f; save(f.root+"/one.txt","original"); OrganizationStore store(f.journal); MoveService moves(store);
    const auto link=f.temp.path()+"/linked";
    REQUIRE(QProcess::execute("cmd.exe",{"/d","/c","mklink","/J",QDir::toNativeSeparators(link),QDir::toNativeSeparators(f.root)})==0);
    struct LinkCleanup { QString path; ~LinkCleanup() { RemoveDirectoryW(path.toStdWString().c_str()); } } cleanup{link};
    CHECK_THROWS(moves.prepare(link+"/one.txt",f.target)); CHECK_THROWS(moves.prepare(f.root+"/one.txt",link));
    CHECK(read(f.root+"/one.txt")=="original"); CHECK_FALSE(QFileInfo(f.target+"/one.txt").exists());
}
TEST_CASE("Recovery refuses a destination copy modified after an interrupted publish") {
    Fixture f; save(f.root+"/one.txt","original"); OrganizationStore store(f.journal); MoveService moves(store); moves.forceCopyForTesting=true;
    const auto plan=moves.prepare(f.root+"/one.txt",f.target);
    moves.checkpoint=[](const QString& stage) { if(stage=="published") throw std::runtime_error("interrupt"); };
    CHECK_THROWS(moves.execute(plan["id"].toString())); changePreservingWriteTime(f.target+"/one.txt");
    MoveService recovery(store); recovery.recover(); CHECK(store.operation(plan["id"].toString())["state"]=="needs_review");
    CHECK_THROWS(recovery.execute(plan["id"].toString())); CHECK(read(f.root+"/one.txt")=="original");
}
TEST_CASE("Copy verify delete protocol and undo preserve contents") {
    Fixture f; const QByteArray contents(2*1024*1024,'a'); save(f.root+"/large.txt",contents);
    OrganizationStore store(f.journal); MoveService moves(store); moves.forceCopyForTesting=true;
    const auto plan=moves.prepare(f.root+"/large.txt",f.target); moves.execute(plan["id"].toString());
    REQUIRE(store.operation(plan["id"].toString())["state"]=="completed"); CHECK(read(f.target+"/large.txt")==contents); CHECK_FALSE(QFileInfo(f.root+"/large.txt").exists());
    const auto undo=moves.prepareUndo(plan["id"].toString()); moves.execute(undo["id"].toString()); CHECK(read(f.root+"/large.txt")==contents); CHECK_FALSE(QFileInfo(f.target+"/large.txt").exists());
}
TEST_CASE("Every mutation boundary leaves a recoverable copy and recovery does not mutate files") {
    const QStringList stages{"executing","renamed","temporary_created","copying","partial_copy","publishing","published","removing","source_removed"};
    for(const auto& stage:stages) {
        CAPTURE(stage); Fixture f; const QByteArray bytes(2*1024*1024,'s'); save(f.root+"/sample.txt",bytes); OrganizationStore store(f.journal); MoveService moves(store);
        moves.forceCopyForTesting=stage!="renamed" && stage!="executing";
        const auto plan=moves.prepare(f.root+"/sample.txt",f.target); const auto id=plan["id"].toString();
        moves.checkpoint=[stage](const QString& point) { if(point==stage) throw std::runtime_error("Simulated process interruption"); };
        CHECK_THROWS(moves.execute(id)); const bool sourceBefore=QFileInfo(f.root+"/sample.txt").exists(),destBefore=QFileInfo(f.target+"/sample.txt").exists();
        OrganizationStore restart(f.journal); MoveService recovery(restart); recovery.recover();
        CHECK(QFileInfo(f.root+"/sample.txt").exists()==sourceBefore); CHECK(QFileInfo(f.target+"/sample.txt").exists()==destBefore);
        CHECK((read(f.root+"/sample.txt")==bytes || read(f.target+"/sample.txt")==bytes));
        const auto state=restart.operation(id)["state"].toString();
        if(stage=="renamed" || stage=="source_removed") CHECK(state=="completed");
        else if(stage=="published" || stage=="removing") { REQUIRE(state=="copy_ready"); recovery.execute(id); CHECK_FALSE(QFileInfo(f.root+"/sample.txt").exists()); CHECK(read(f.target+"/sample.txt")==bytes); }
        else CHECK(state=="needs_review");
    }
}
TEST_CASE("Repository files, index data, hard links and lossy stream copies are refused") {
    Fixture f; f.add("one.txt","original"); OrganizationStore store(f.journal); MoveService moves(store,{f.data});
    save(f.data+"/private.txt","index"); CHECK_THROWS(moves.prepare(f.data+"/private.txt",f.target));
    REQUIRE(QDir().mkpath(f.root+"/.git")); CHECK_THROWS(moves.prepare(f.root+"/one.txt",f.target)); REQUIRE(QDir().rmdir(f.root+"/.git"));
    REQUIRE(CreateHardLinkW((f.root+"/linked.txt").toStdWString().c_str(),(f.root+"/one.txt").toStdWString().c_str(),nullptr)); CHECK_THROWS(moves.prepare(f.root+"/one.txt",f.target));
    save(f.root+"/streams.txt","content"); save(f.root+"/streams.txt:extra","stream data"); auto plan=moves.prepare(f.root+"/streams.txt",f.target); moves.forceCopyForTesting=true; CHECK_THROWS(moves.execute(plan["id"].toString())); CHECK(read(f.root+"/streams.txt:extra")=="stream data");
}
TEST_CASE("Abrupt process death preserves durable move intent and supports inspected recovery") {
    for(const auto& stage:QStringList{"renamed","published","source_removed"}) {
        CAPTURE(stage); Fixture f; save(f.root+"/crash.txt","durable contents");
        QProcess child; child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({"--crash-move",f.journal,f.root+"/crash.txt",f.target,stage}); child.start();
        REQUIRE(child.waitForStarted(3000)); REQUIRE(child.waitForFinished(15000)); CHECK(child.exitCode()==44);
        OrganizationStore store(f.journal); REQUIRE(store.history().size()==1); const auto id=store.history().front().toMap()["id"].toString();
        const bool source=QFileInfo(f.root+"/crash.txt").exists(); MoveService(store).recover();
        CHECK(QFileInfo(f.root+"/crash.txt").exists()==source); CHECK(read(f.target+"/crash.txt")=="durable contents");
        CHECK(store.operation(id)["state"]==(stage=="published"?"copy_ready":"completed"));
    }
}
TEST_CASE("Undo refuses a substituted original folder and recovery completes an interrupted undo") {
    Fixture f; save(f.root+"/one.txt","contents"); OrganizationStore store(f.journal); MoveService moves(store);
    const auto plan=moves.prepare(f.root+"/one.txt",f.target); moves.execute(plan["id"].toString());
    REQUIRE(QDir().rename(f.root,f.root+"-old")); REQUIRE(QDir().mkpath(f.root)); CHECK_THROWS(moves.prepareUndo(plan["id"].toString()));
    REQUIRE(QDir().rmdir(f.root)); REQUIRE(QDir().rename(f.root+"-old",f.root));
    const auto undo=moves.prepareUndo(plan["id"].toString()); moves.checkpoint=[](const QString& stage) { if(stage=="renamed") throw std::runtime_error("interrupt undo"); };
    CHECK_THROWS(moves.execute(undo["id"].toString())); MoveService recovery(store); recovery.recover();
    CHECK(store.operation(plan["id"].toString())["state"]=="undone"); CHECK(read(f.root+"/one.txt")=="contents");
}
TEST_CASE("Controller reviews moves and category decisions without moving on suggestion acceptance") {
    Fixture f; const auto id=f.add("diodes.txt","Semiconductor diode rectifier circuits conduct electricity.");
    OrganizationController controller(f.data); QCoreApplication::processEvents(); waitFor([&]{return !controller.busy();});
    controller.saveCategory(0,"Circuits","semiconductor diode rectifier",f.target); waitFor([&]{return !controller.busy();}); REQUIRE(controller.categories().size()==1);
    controller.inspect(id); waitFor([&]{return !controller.busy();}); INFO(controller.message().toStdString()); REQUIRE(controller.suggestions().size()==1);
    const auto category=controller.categories().front().toMap()["id"].toLongLong(); controller.decide(category,true); waitFor([&]{return !controller.busy();}); CHECK(QFileInfo(f.root+"/diodes.txt").exists());
    controller.prepareMove(f.target); waitFor([&]{return !controller.busy();}); REQUIRE(!controller.movePlan().isEmpty()); CHECK(QFileInfo(f.root+"/diodes.txt").exists());
    controller.approveMove(controller.movePlan()["id"].toString()); waitFor([&]{return !controller.busy();}); INFO(controller.message().toStdString()); CHECK(QFileInfo(f.target+"/diodes.txt").exists());
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    const auto args=app.arguments();
    if(args.size()==6 && args[1]=="--crash-move") {
        OrganizationStore store(args[2]); MoveService move(store); move.forceCopyForTesting=args[5]!="renamed";
        const auto plan=move.prepare(args[3],args[4]);
        move.checkpoint=[stage=args[5]](const QString& point) { if(point==stage) TerminateProcess(GetCurrentProcess(),44); };
        move.execute(plan["id"].toString()); return 2;
    }
    return Catch::Session().run(argc,argv);
}
