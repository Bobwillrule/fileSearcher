#include "app/app_controller.h"
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

namespace {
void settled(AppController& controller) {
    QElapsedTimer timer; timer.start();
    while ((controller.busy() || controller.searching()) && timer.elapsed() < 10000) {
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    REQUIRE_FALSE(controller.busy()); REQUIRE_FALSE(controller.searching());
}
void write(const QString& path) {
    QFile file(path); REQUIRE(file.open(QIODevice::WriteOnly)); REQUIRE(file.write("synthetic fixture") == 17);
}
}
TEST_CASE("Desktop controller scans asynchronously, persists results and validates actions") {
    QTemporaryDir fixture; REQUIRE(fixture.isValid());
    const auto data = fixture.path() + "/index", documents = fixture.path() + "/documents";
    REQUIRE(QDir().mkpath(documents));
    write(documents + "/lecture.txt"); write(documents + "/main.cpp"); write(documents + "/ignore.tmp");
    qint64 root = 0;
    {
        AppController controller(data,nullptr,{500,100,300000,true,false}); settled(controller); REQUIRE(controller.error().isEmpty());
        controller.addFolder(QUrl::fromLocalFile(documents), "*.tmp");
        REQUIRE(controller.busy()); settled(controller);
        INFO(controller.error().toStdString()); REQUIRE(controller.error().isEmpty()); REQUIRE(controller.roots().size() == 1);
        root = controller.roots()[0].toMap()["rootId"].toLongLong();
        REQUIRE(controller.total() == 2);
        controller.search("lecture","",0,"","",0);
        controller.search("main","cpp",root,"1","100",1);
        settled(controller); REQUIRE(controller.total() == 1);
        CHECK(controller.results()[0].toMap()["name"].toString() == "main.cpp");
        controller.search("","",0,"20","10",0); settled(controller);
        CHECK_FALSE(controller.error().isEmpty()); CHECK(controller.total() == 0);
        controller.scan(); settled(controller); CHECK(controller.total() == 0);
        controller.search("","",0," ","",0); settled(controller); CHECK(controller.total() == 2);
        controller.changeExclusions(root,"*.tmp, *.cpp"); settled(controller); CHECK(controller.total() == 1);
        controller.changeExclusions(root,"*.tmp"); settled(controller); CHECK(controller.total() == 2);
    }
    AppController restarted(data,nullptr,{500,100,300000,true,false}); settled(restarted);
    REQUIRE(restarted.total() == 2); REQUIRE(restarted.roots().size() == 1);
    restarted.search("lecture","",0,"","",0); settled(restarted); REQUIRE(restarted.total() == 1);
    const auto id = restarted.results()[0].toMap()["fileId"].toLongLong();
    REQUIRE(QFile::remove(documents + "/lecture.txt"));
    restarted.openFile(id,false); CHECK(restarted.error().contains("no longer available"));
    restarted.openFile(id,true); CHECK(restarted.error().contains("no longer available"));
    restarted.scan(); settled(restarted); CHECK(restarted.total() == 0);
    restarted.removeFolder(root); settled(restarted); CHECK(restarted.roots().isEmpty());
    CHECK(QFileInfo::exists(documents + "/main.cpp"));
    restarted.addFolder(QUrl::fromLocalFile(data),""); CHECK_FALSE(restarted.error().isEmpty());
    CHECK(restarted.roots().isEmpty());
}
TEST_CASE("Closing the controller safely cancels a queued scan") {
    QTemporaryDir fixture; REQUIRE(fixture.isValid());
    auto documents = fixture.path() + "/documents"; REQUIRE(QDir().mkpath(documents));
    for (int i = 0; i < 200; ++i) write(documents + QString("/file-%1.txt").arg(i));
    {
        AppController controller(fixture.path() + "/index",nullptr,{500,100,300000,false});
        controller.addFolder(QUrl::fromLocalFile(documents),"");
        controller.cancel();
    }
    AppController restarted(fixture.path() + "/index",nullptr,{500,100,300000,false}); settled(restarted);
    REQUIRE(restarted.paused()); restarted.setPaused(false); settled(restarted); CHECK(restarted.total() == 200);
}
int main(int argc, char* argv[]) { QCoreApplication app(argc,argv); return Catch::Session().run(argc,argv); }
