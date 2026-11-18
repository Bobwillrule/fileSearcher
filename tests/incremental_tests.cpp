#include "app/app_controller.h"
#include "indexing/work_queue.h"
#include <catch2/catch_test_macros.hpp>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QSqlQuery>
#include <functional>

namespace {
void waitUntil(const std::function<bool()>& predicate, int timeout = 12000) {
    QElapsedTimer timer; timer.start();
    do { QCoreApplication::processEvents(); if (predicate()) return; QThread::msleep(2); } while (timer.elapsed() < timeout);
    REQUIRE(predicate());
}
void pump(int milliseconds) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < milliseconds) { QCoreApplication::processEvents(); QThread::msleep(2); }
}
void writeFixture(const QString& path, const QByteArray& bytes = "fixture") {
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path); REQUIRE(file.open(QIODevice::WriteOnly)); REQUIRE(file.write(bytes) == bytes.size());
}
bool idle(const AppController& controller) { return !controller.busy() && !controller.searching() && controller.pending() == 0; }
constexpr IndexingTiming fast{60,10,60000,false}; // Metadata stress tests isolate watcher/queue behavior.
}
TEST_CASE("Durable work coalesces, survives restart and retains events arriving during a scan") {
    QTemporaryDir fixture; REQUIRE(fixture.isValid()); auto database = fixture.path()+"/work.sqlite";
    {
        filesearcher::WorkQueue queue(database);
        queue.enqueue(1,"root",0,500); queue.enqueue(1,"root",100,500);
        CHECK(queue.pending() == 1); CHECK_FALSE(queue.take(499));
        auto work = queue.take(500); REQUIRE(work); CHECK_FALSE(queue.take(500));
        queue.enqueue(1,"root",510,500);
        queue.finish(*work,true,false,520);
        CHECK(queue.pending() == 1);
        auto followup = queue.take(600); REQUIRE(followup);
        queue.setPaused(true); // Simulate shutdown/crash with work still claimed.
    }
    filesearcher::WorkQueue queue(database); CHECK(queue.paused());
    auto recovered = queue.take(700); REQUIRE(recovered);
    queue.finish(*recovered,true,false,700); CHECK(queue.pending() == 0);
    queue.enqueue(2,"removed",0,0); queue.retain({1}); CHECK(queue.pending() == 0);
}
TEST_CASE("Failed work has bounded retries and cancellation does not consume an attempt") {
    QTemporaryDir fixture; filesearcher::WorkQueue queue(fixture.path()+"/work.sqlite");
    queue.enqueue(1,"root",0,0);
    auto first = queue.take(0); REQUIRE(first);
    queue.finish(*first,false,true,0);
    auto again = queue.take(0); REQUIRE(again); CHECK(again->attempts == 0);
    queue.finish(*again,false,false,0); CHECK_FALSE(queue.take(999));
    auto second = queue.take(1000); REQUIRE(second); CHECK(second->attempts == 1);
    queue.finish(*second,false,false,1000); CHECK_FALSE(queue.take(2999));
    auto third = queue.take(3000); REQUIRE(third); queue.finish(*third,false,false,3000);
    CHECK_FALSE(queue.take(100000)); CHECK(queue.pending() == 1);
    queue.enqueue(1,"root",100001,0); REQUIRE(queue.take(100001));
}
TEST_CASE("Windows notifications converge after nested creates rapid saves renames and deletes") {
    QTemporaryDir fixture; const auto root = fixture.path()+"/docs";
    writeFixture(root+"/original.txt");
    AppController controller(fixture.path()+"/index",nullptr,fast);
    controller.addFolder(QUrl::fromLocalFile(root),"*.tmp, ignored");
    waitUntil([&] { return idle(controller) && controller.total() == 1; });
    REQUIRE(controller.monitoring() == "Automatic updates enabled");
    for (int i=0;i<120;++i) writeFixture(root+QString("/new/sub/file-%1.txt").arg(i));
    for (int i=0;i<20;++i) writeFixture(root+"/original.txt",QByteArray(100+i,'x'));
    REQUIRE(QFile::rename(root+"/original.txt",root+"/renamed.txt"));
    for (int i=0;i<40;++i) REQUIRE(QFile::remove(root+QString("/new/sub/file-%1.txt").arg(i)));
    writeFixture(root+"/ignored/skip.txt"); writeFixture(root+"/skip.tmp");
    waitUntil([&] { return idle(controller) && controller.total() == 81; });
    controller.search("renamed","",0,"","",0);
    waitUntil([&] { return !controller.searching(); }); REQUIRE(controller.total() == 1);
    CHECK(controller.results().front().toMap()["size"].toLongLong() == 119);
    REQUIRE(QDir().rename(root+"/new",root+"/moved"));
    controller.search("moved/sub","",0,"","",0);
    waitUntil([&] { return idle(controller) && controller.total() == 80; });
    controller.search("original.txt","",0,"","",0);
    waitUntil([&] { return !controller.searching(); }); CHECK(controller.total() == 0);
}
TEST_CASE("Pause persists across restart and changes made offline are reconciled on resume") {
    QTemporaryDir fixture; const auto root = fixture.path()+"/docs", data = fixture.path()+"/index";
    writeFixture(root+"/before.txt");
    {
        AppController controller(data,nullptr,fast); controller.addFolder(QUrl::fromLocalFile(root),"");
        waitUntil([&] { return idle(controller) && controller.total() == 1; });
        controller.setPaused(true);
        writeFixture(root+"/during-pause.txt");
        waitUntil([&] { return controller.pending() > 0; });
        CHECK(controller.total() == 1); CHECK_FALSE(controller.busy());
    }
    REQUIRE(QFile::remove(root+"/before.txt")); writeFixture(root+"/while-closed.txt");
    {
        AppController controller(data,nullptr,fast); REQUIRE(controller.paused());
        waitUntil([&] { return !controller.searching(); }); CHECK(controller.total() == 1);
        controller.setPaused(false);
        waitUntil([&] { return idle(controller) && controller.total() == 2; });
    }
    writeFixture(root+"/another-offline.txt");
    AppController controller(data,nullptr,fast);
    waitUntil([&] { return idle(controller) && controller.total() == 3; });
}
TEST_CASE("Unavailable folders retain records and periodic checks recover a replaced root") {
    QTemporaryDir fixture; const auto root = fixture.path()+"/docs", away = fixture.path()+"/away";
    writeFixture(root+"/old.txt");
    AppController controller(fixture.path()+"/index",nullptr,{60,10,800,false});
    controller.addFolder(QUrl::fromLocalFile(root),"");
    waitUntil([&] { return idle(controller) && controller.total() == 1; });
    REQUIRE(QDir().rename(root,away));
    waitUntil([&] { return controller.pending()>0 && !controller.busy(); });
    CHECK(controller.total() == 1);
    writeFixture(root+"/replacement.txt");
    controller.search("replacement","",0,"","",0);
    waitUntil([&] { return idle(controller) && controller.total() == 1; });
    writeFixture(root+"/new-after-replacement.txt");
    controller.search("","",0,"","",0);
    waitUntil([&] { return idle(controller) && controller.total() == 2; });
    CHECK(QFileInfo::exists(away+"/old.txt"));
}
TEST_CASE("Overlapping watched roots remain unique and removing roots discards pending work only") {
    QTemporaryDir fixture; const auto root = fixture.path()+"/docs", sub = root+"/sub";
    writeFixture(sub+"/first.txt");
    AppController controller(fixture.path()+"/index",nullptr,fast);
    controller.addFolder(QUrl::fromLocalFile(root),""); waitUntil([&] { return idle(controller); });
    controller.addFolder(QUrl::fromLocalFile(sub),""); waitUntil([&] { return idle(controller); });
    writeFixture(sub+"/second.txt"); waitUntil([&] { return idle(controller) && controller.total() == 2; });
    controller.setPaused(true);
    writeFixture(sub+"/third.txt"); waitUntil([&] { return controller.pending() == 2; });
    auto roots = controller.roots();
    for (const auto& entry : roots) controller.removeFolder(entry.toMap()["rootId"].toLongLong());
    waitUntil([&] { return !controller.searching(); });
    CHECK(controller.pending() == 0); CHECK(controller.total() == 0);
    controller.setPaused(false); pump(150); CHECK(controller.total() == 0);
    CHECK(QFileInfo::exists(sub+"/third.txt"));
}
TEST_CASE("Watching a parent of the index does not cause self-triggered scan loops") {
    QTemporaryDir fixture; const auto root = fixture.path()+"/docs";
    writeFixture(root+"/first.txt");
    AppController controller(root+"/local-index",nullptr,fast);
    controller.addFolder(QUrl::fromLocalFile(root),"");
    waitUntil([&] { return idle(controller) && controller.total() == 1; });
    int newScans = 0; bool wasBusy = false;
    QObject::connect(&controller,&AppController::stateChanged,&controller,[&] {
        if (controller.busy() && !wasBusy) ++newScans;
        wasBusy = controller.busy();
    });
    pump(800); CHECK(newScans == 0); CHECK(controller.pending() == 0);
    writeFixture(root+"/second.txt"); waitUntil([&] { return idle(controller) && controller.total() == 2; });
}
TEST_CASE("A second writer cannot open the same index") {
    QTemporaryDir fixture;
    AppController first(fixture.path());
    AppController second(fixture.path());
    CHECK(second.error().contains("already open"));
}
TEST_CASE("Notification overflow schedules full reconciliation instead of losing changes") {
    QTemporaryDir fixture; const auto root = fixture.path()+"/docs"; REQUIRE(QDir().mkpath(root));
    filesearcher::DirectoryWatcher watcher;
    int overflow = 0, dirty = 0;
    QObject::connect(&watcher,&filesearcher::DirectoryWatcher::overflowed,&watcher,[&](qint64) { ++overflow; });
    QObject::connect(&watcher,&filesearcher::DirectoryWatcher::dirty,&watcher,[&](qint64) { ++dirty; });
    watcher.synchronize({{1,root,{},""}},{}); REQUIRE(watcher.unavailable().isEmpty());
    // Hold the event loop while changes exceed the fixed 64 KiB native buffer.
    for (int i=0;i<1600;++i) writeFixture(root+QString("/overflow-%1-%2.txt").arg(i).arg(QString(80,'x')));
    waitUntil([&] { return overflow > 0; }); CHECK(dirty > 0);
}
TEST_CASE("Changes arriving while a scan runs receive another pass") {
    QTemporaryDir fixture; const auto root = fixture.path()+"/docs";
    for (int i=0;i<600;++i) writeFixture(root+QString("/file-%1.txt").arg(i));
    AppController controller(fixture.path()+"/index",nullptr,fast);
    bool added = false;
    QObject::connect(&controller,&AppController::stateChanged,&controller,[&] {
        if (!added && controller.busy() && controller.status().startsWith("Checking changes:")) {
            added = true; writeFixture(root+"/arrived-during-scan.txt");
        }
    });
    controller.addFolder(QUrl::fromLocalFile(root),"");
    waitUntil([&] { return idle(controller) && controller.total() == 601; }); REQUIRE(added);
}
