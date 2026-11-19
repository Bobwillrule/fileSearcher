#include "app/app_controller.h"
#include <catch2/catch_test_macros.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include "storage/embedding_store.h"

namespace {
void waitFor(const std::function<bool()>& predicate) {
    QElapsedTimer timer; timer.start();
    while(timer.elapsed()<20000) { QCoreApplication::processEvents(); if(predicate()) return; QThread::msleep(2); }
    REQUIRE(predicate());
}
bool finished(const AppController& controller) { return !controller.busy() && !controller.searching() && controller.pending()==0; }
void save(const QString& path,const QByteArray& text) {
    QFile file(path); REQUIRE(file.open(QIODevice::WriteOnly)); REQUIRE(file.write(text)==text.size());
}
}
TEST_CASE("Desktop content search and plain-text preview update after live edits") {
    QTemporaryDir fixture; const auto root=fixture.path()+"/docs"; REQUIRE(QDir().mkpath(root));
    save(root+"/lecture19.md","Master Theorem describes divide and conquer. <img src=x onerror=alert(1)>");
    save(root+"/diodes.txt","Rectifier circuits convert alternating current.");
    AppController controller(fixture.path()+"/index",nullptr,{60,10,60000,true,false});
    QTimer heartbeat; int ticks=0; QObject::connect(&heartbeat,&QTimer::timeout,[&] { ++ticks; }); heartbeat.start(5);
    controller.addFolder(QUrl::fromLocalFile(root),"");
    waitFor([&] { return finished(controller); }); REQUIRE(ticks>2); REQUIRE(controller.error().isEmpty());
    controller.search("Master Theorem","",0,"","",0,3);
    waitFor([&] { return !controller.searching(); }); REQUIRE(controller.total()==1);
    auto row=controller.results().front().toMap();
    controller.loadPreview(row["fileId"].toLongLong(),row["passageId"].toLongLong());
    waitFor([&] { return !controller.previewLoading(); });
    CHECK(controller.preview()["text"].toString().contains("<img src=x onerror=alert(1)>"));
    save(root+"/lecture19.md","Replacement material explains dynamic programming in more detail.");
    waitFor([&] { return finished(controller) && controller.total()==0; });
    controller.search("dynamic programming","",0,"","",0,3);
    waitFor([&] { return !controller.searching(); }); REQUIRE(controller.total()==1);
    row=controller.results().front().toMap(); controller.loadPreview(row["fileId"].toLongLong(),row["passageId"].toLongLong());
    waitFor([&] { return !controller.previewLoading(); }); CHECK_FALSE(controller.preview()["text"].toString().contains("Master Theorem"));
    controller.search("\"","",0,"","",0,4);
    waitFor([&] { return !controller.searching(); }); CHECK_FALSE(controller.error().isEmpty()); CHECK(controller.total()==0);
    controller.search("Rectifier","txt",0,"","",0,2);
    waitFor([&] { return !controller.searching(); }); CHECK(controller.error().isEmpty()); CHECK(controller.total()==1);
    controller.setPaused(true); controller.reindexContent();
    waitFor([&] { return !controller.searching(); }); CHECK(controller.total()==0); CHECK(controller.pending()>0);
    controller.setPaused(false); waitFor([&] { return finished(controller) && controller.total()==1; });
}
TEST_CASE("Desktop AI indexing honors saved settings and resumes without re-embedding", "[.model]") {
    QTemporaryDir fixture; const auto root=fixture.path()+"/docs",data=fixture.path()+"/index";
    REQUIRE(QDir().mkpath(root)); save(root+"/lecture.md","Master Theorem explains divide and conquer recurrence relations.");
    qint64 generation=0;
    {
        AppController controller(data,nullptr,{60,10,60000,true,true});
        controller.configureAi(false,0); controller.addFolder(QUrl::fromLocalFile(root),"");
        waitFor([&] { return finished(controller); });
        controller.search("recurrence","",0,"","",0,2); waitFor([&] { return !controller.searching(); }); CHECK(controller.total()==1);
        filesearcher::EmbeddingStore store(data+"/metadata.sqlite"); CHECK(store.count()==0);
        controller.configureAi(true,0); waitFor([&] { return finished(controller); });
        INFO(controller.aiStatus().toStdString()); REQUIRE(controller.aiStatus().contains("Local AI ready")); CHECK(store.count()==1);
        generation=store.generation(); controller.configureAi(true,2); waitFor([&] { return finished(controller); }); CHECK(store.generation()==generation);
    }
    AppController restarted(data,nullptr,{60,10,60000,true,true}); waitFor([&] { return finished(restarted); });
    CHECK(restarted.aiEnabled()); CHECK(restarted.aiProfile()==2); CHECK(restarted.aiStatus().contains("0 new"));
    filesearcher::EmbeddingStore store(data+"/metadata.sqlite"); CHECK(store.generation()==generation);
    restarted.configureAi(false,1); CHECK_FALSE(restarted.aiEnabled()); CHECK(restarted.aiStatus().contains("off"));
}
TEST_CASE("Desktop hybrid queries find paraphrases and discard superseded requests", "[.model]") {
    QTemporaryDir fixture; const auto root=fixture.path()+"/docs",data=fixture.path()+"/index";
    REQUIRE(QDir().mkpath(root));
    save(root+"/diodes.txt","A semiconductor diode conducts current in the forward direction and blocks reverse current. A rectifier converts alternating current into direct current.");
    save(root+"/lecture.md","The Master Theorem solves divide and conquer recurrence relations and bounds recursive algorithm complexity.");
    AppController controller(data,nullptr,{60,10,60000,true,true});
    controller.addFolder(QUrl::fromLocalFile(root),""); waitFor([&] { return finished(controller); });
    controller.search("Which component lets electricity pass in just one direction?","",0,"","",0,5);
    waitFor([&] { return !controller.searching(); }); INFO(controller.error().toStdString()); REQUIRE(controller.error().isEmpty());
    REQUIRE(controller.total()>0); auto row=controller.results().front().toMap();
    CHECK(row["name"].toString()=="diodes.txt"); CHECK(row["matchLabel"].toString().contains("Similar meaning"));
    controller.loadPreview(row["fileId"].toLongLong(),row["passageId"].toLongLong()); waitFor([&] { return !controller.previewLoading(); });
    CHECK(controller.preview()["text"].toString().contains("semiconductor"));
    controller.search("recursion running time","",0,"","",0,5);
    QCoreApplication::processEvents(); QThread::msleep(80);
    controller.search("diodes.txt","txt",0,"","",0,5);
    waitFor([&] { return !controller.searching(); }); REQUIRE(controller.total()==1);
    CHECK(controller.results().front().toMap()["name"].toString()=="diodes.txt");
    controller.search("one direction","pdf",0,"","",0,5); waitFor([&] { return !controller.searching(); }); CHECK(controller.total()==0);
    controller.search("diode","",0,"","",0,5); controller.configureAi(false,1);
    waitFor([&] { return !controller.searching(); }); CHECK(controller.searchNotice().contains("off")); REQUIRE(controller.total()==1);
    CHECK_FALSE(controller.results().front().toMap()["matchLabel"].toString().contains("Similar meaning"));
}
TEST_CASE("Clearing saved search data preserves original files and supports re-adding folders") {
    QTemporaryDir fixture; const auto root=fixture.path()+"/docs",data=fixture.path()+"/index";
    REQUIRE(QDir().mkpath(root)); save(root+"/keep.txt","Original contents must survive index maintenance.");
    AppController controller(data,nullptr,{60,10,60000,true,false}); controller.addFolder(QUrl::fromLocalFile(root),"");
    waitFor([&] { return finished(controller); });
    controller.refreshStorage(); waitFor([&] { return controller.storageInfo().contains("files"); }); CHECK(controller.storageInfo()["files"].toInt()==1);
    controller.setPaused(true); controller.clearSavedIndex(true); waitFor([&] { return !controller.busy(); });
    CHECK(controller.roots().size()==1); REQUIRE(QFileInfo::exists(root+"/keep.txt"));
    controller.clearSavedIndex(false); waitFor([&] { return !controller.busy() && !controller.searching(); });
    CHECK(controller.roots().isEmpty()); CHECK(controller.total()==0); CHECK(controller.error().isEmpty());
    QFile original(root+"/keep.txt"); REQUIRE(original.open(QIODevice::ReadOnly)); CHECK(original.readAll()=="Original contents must survive index maintenance."); original.close();
    controller.setPaused(false); controller.addFolder(QUrl::fromLocalFile(root),""); waitFor([&] { return finished(controller); }); CHECK(controller.total()==1);
}

TEST_CASE("Approved moves reconcile watched folders and undo while monitoring stays active") {
    QTemporaryDir fixture; const auto root=fixture.path()+"/docs",target=fixture.path()+"/organized";
    REQUIRE(QDir().mkpath(root)); REQUIRE(QDir().mkpath(target)); save(root+"/keep.txt","Original monitored file.");
    AppController controller(fixture.path()+"/index",nullptr,{30,10,60000,true,false});
    controller.addFolder(QUrl::fromLocalFile(root),""); waitFor([&]{return finished(controller);});
    controller.addFolder(QUrl::fromLocalFile(target),""); waitFor([&]{return finished(controller);});
    auto* organizer=qobject_cast<OrganizationController*>(controller.organizer()); REQUIRE(organizer);
    waitFor([&]{return !organizer->busy();}); REQUIRE(controller.total()==1);
    organizer->inspect(controller.results().front().toMap()["fileId"].toLongLong()); waitFor([&]{return !organizer->busy();});
    organizer->prepareMove(target); waitFor([&]{return !organizer->busy();}); INFO(organizer->message().toStdString()); REQUIRE(!organizer->movePlan().isEmpty());
    const auto moveId=organizer->movePlan()["id"].toString(); organizer->approveMove(moveId); waitFor([&]{return !organizer->busy() && finished(controller);});
    INFO(organizer->message().toStdString()); REQUIRE(QFileInfo::exists(target+"/keep.txt"));
    waitFor([&]{return controller.total()==1 && controller.results().front().toMap()["path"].toString()==target+"/keep.txt";});
    organizer->prepareUndo(moveId); waitFor([&]{return !organizer->busy();}); REQUIRE(!organizer->movePlan().isEmpty());
    organizer->approveMove(organizer->movePlan()["id"].toString()); waitFor([&]{return !organizer->busy() && finished(controller);});
    REQUIRE(QFileInfo::exists(root+"/keep.txt"));
    waitFor([&]{return controller.total()==1 && controller.results().front().toMap()["path"].toString()==root+"/keep.txt";});
}

TEST_CASE("Real BGE passages drive related files and example-based category suggestions", "[.model]") {
    QTemporaryDir fixture; const auto root=fixture.path()+"/docs"; REQUIRE(QDir().mkpath(root));
    save(root+"/diode-principles.txt","A semiconductor diode conducts current in the forward direction and blocks reverse current. A rectifier converts alternating current into direct current.");
    save(root+"/rectifier-reference.txt","Diodes pass current in one direction and block it in the other. Rectifier circuits convert AC power into DC power.");
    save(root+"/banana.txt","Bananas and apples are fruits used to make sweet desserts and fruit salads.");
    AppController controller(fixture.path()+"/index",nullptr,{30,10,60000,true,true}); controller.addFolder(QUrl::fromLocalFile(root),"");
    waitFor([&]{return finished(controller);}); REQUIRE(controller.aiStatus().contains("Local AI ready"));
    qint64 selected=0,example=0;
    for(const auto& value:controller.results()) { const auto row=value.toMap(); if(row["name"]=="diode-principles.txt") selected=row["fileId"].toLongLong(); if(row["name"]=="rectifier-reference.txt") example=row["fileId"].toLongLong(); }
    REQUIRE(selected); REQUIRE(example);
    auto* organizer=qobject_cast<OrganizationController*>(controller.organizer()); REQUIRE(organizer); waitFor([&]{return !organizer->busy();});
    organizer->saveCategory(0,"Components","Laboratory examples",QStringLiteral("")); waitFor([&]{return !organizer->busy();}); REQUIRE(organizer->categories().size()==1);
    organizer->inspect(example); waitFor([&]{return !organizer->busy();});
    organizer->addExample(organizer->categories().front().toMap()["id"].toLongLong()); waitFor([&]{return !organizer->busy();});
    organizer->inspect(selected); waitFor([&]{return !organizer->busy();}); INFO(organizer->message().toStdString());
    REQUIRE(!organizer->related().isEmpty()); CHECK(organizer->related().front().toMap()["fileId"].toLongLong()==example);
    REQUIRE(organizer->suggestions().size()==1); CHECK(organizer->suggestions().front().toMap()["reason"].toString().contains("example"));
    auto* cleanup=qobject_cast<CleanupController*>(controller.cleanup()); REQUIRE(cleanup); waitFor([&]{return !cleanup->busy();});
    cleanup->inspect(selected); waitFor([&]{return !cleanup->busy();});
    REQUIRE(!cleanup->related().isEmpty()); CHECK(cleanup->related().front().toMap()["kind"]=="Similar document"); CHECK(cleanup->plan().isEmpty());
    controller.configureAi(false,1); organizer->inspect(selected); waitFor([&]{return !organizer->busy();});
    CHECK(organizer->suggestions().isEmpty()); CHECK(organizer->related().isEmpty());
    cleanup->inspect(selected); waitFor([&]{return !cleanup->busy();}); CHECK(cleanup->related().isEmpty());
}
