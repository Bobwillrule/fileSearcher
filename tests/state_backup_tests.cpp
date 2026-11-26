#include "storage/state_backup.h"
#include "storage/metadata_store.h"
#include "organization/organization_store.h"
#include "indexing/work_queue.h"
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QJsonArray>
#define NOMINMAX
#include <windows.h>
using namespace filesearcher;
namespace {
void save(const QString& path,const QByteArray& data) { QFile f(path); REQUIRE(f.open(QIODevice::WriteOnly)); REQUIRE(f.write(data)==data.size()); }
void legacy(const QString& directory) {
    { auto db=QSqlDatabase::addDatabase("QSQLITE","legacy"); db.setDatabaseName(directory+"/metadata.sqlite"); REQUIRE(db.open());
      { QSqlQuery q(db); REQUIRE(q.exec("CREATE TABLE files(id INTEGER PRIMARY KEY AUTOINCREMENT,path TEXT NOT NULL,path_key TEXT NOT NULL UNIQUE,name TEXT NOT NULL,name_key TEXT NOT NULL,extension TEXT NOT NULL,size INTEGER NOT NULL,modified INTEGER NOT NULL)")); REQUIRE(q.exec("INSERT INTO files VALUES(7,'C:/synthetic.txt','c:/synthetic.txt','synthetic.txt','synthetic.txt','txt',8,123)")); REQUIRE(q.exec("PRAGMA user_version=1")); } db.close(); }
    QSqlDatabase::removeDatabase("legacy");
    OrganizationStore organization(directory+"/organization.sqlite"); organization.saveCategory(0,"Saved collection","Keep this","");
    WorkQueue queue(directory+"/work.sqlite"); queue.setPaused(true); save(directory+"/preferences.ini","[ai]\nenabled=false\n");
}
}
TEST_CASE("Upgrade backup restores legacy database, collections, queue and preferences") {
    QTemporaryDir temp; legacy(temp.path()); const auto backup=prepareStateUpgrade(temp.path()); REQUIRE(!backup.isEmpty());
    const auto manifest=verifyStateBackup(backup); CHECK(manifest["files"].toArray().size()==4);
    CHECK(prepareStateUpgrade(temp.path())==backup);
    { MetadataStore upgraded(temp.path()+"/metadata.sqlite",true); CHECK(upgraded.filePath(7)=="C:/synthetic.txt"); }
    finishStateUpgrade(temp.path()); CHECK(prepareStateUpgrade(temp.path()).isEmpty());
    const auto restored=temp.path()+"/restored"; restoreStateBackup(backup,restored);
    { MetadataStore old(restored+"/metadata.sqlite"); CHECK(old.filePath(7)=="C:/synthetic.txt"); OrganizationStore org(restored+"/organization.sqlite"); CHECK(org.categories().size()==1); WorkQueue queue(restored+"/work.sqlite"); CHECK(queue.paused()); }
    REQUIRE_THROWS(restoreStateBackup(backup,restored)); CHECK(QFile::exists(restored+"/preferences.ini"));
}
TEST_CASE("Backup contains committed WAL records while an existing connection remains open") {
    QTemporaryDir temp; MetadataStore metadata(temp.path()+"/metadata.sqlite",true); metadata.addRoot("C:/synthetic",{});
    const auto backup=prepareStateUpgrade(temp.path()); REQUIRE(!backup.isEmpty()); MetadataStore copied(backup+"/metadata.sqlite"); CHECK(copied.roots().size()==1);
}
TEST_CASE("Corrupted and incomplete backups cannot be restored or reused") {
    QTemporaryDir temp; legacy(temp.path()); auto backup=prepareStateUpgrade(temp.path());
    SECTION("altered member") { save(backup+"/preferences.ini","corrupt"); }
    SECTION("missing manifest") { REQUIRE(QFile::remove(backup+"/manifest.json")); }
    REQUIRE_THROWS(verifyStateBackup(backup)); REQUIRE_THROWS(prepareStateUpgrade(temp.path())); REQUIRE_THROWS(restoreStateBackup(backup,temp.path()+"/restore"));
    CHECK_FALSE(QDir(temp.path()+"/restore").exists()); MetadataStore unchanged(temp.path()+"/metadata.sqlite"); CHECK(unchanged.filePath(7)=="C:/synthetic.txt");
}
TEST_CASE("Backup failure stops before migration and retry keeps the original state") {
    QTemporaryDir temp; legacy(temp.path());
    REQUIRE_THROWS(prepareStateUpgrade(temp.path(),[](const QString& stage){if(stage=="copied") throw std::runtime_error("Interrupted copy");}));
    CHECK_FALSE(QFile::exists(temp.path()+"/upgrade.pending.json")); CHECK_FALSE(QFile::exists(temp.path()+"/release-state.json"));
    const auto complete=prepareStateUpgrade(temp.path()); CHECK(verifyStateBackup(complete)["complete"].toBool());
}
TEST_CASE("Future release state and schema are rejected without starting a backup") {
    QTemporaryDir temp; legacy(temp.path());
    SECTION("future marker") { save(temp.path()+"/release-state.json","{\"format\":1,\"release\":\"99.0.0\"}"); }
    SECTION("future schema") { auto db=QSqlDatabase::addDatabase("QSQLITE","future"); db.setDatabaseName(temp.path()+"/metadata.sqlite"); REQUIRE(db.open()); {QSqlQuery q(db);REQUIRE(q.exec("PRAGMA user_version=99"));} db.close();db={};QSqlDatabase::removeDatabase("future"); }
    REQUIRE_THROWS(prepareStateUpgrade(temp.path())); CHECK_FALSE(QDir(temp.path()+"/backups").exists());
}
TEST_CASE("Abrupt upgrade interruption reuses a verified backup and migration remains restartable") {
    for(const auto& stage:{QString("pending_saved"),QString("migration_committed")}) {
        QTemporaryDir temp; legacy(temp.path()); QProcess child;
        child.start(QCoreApplication::applicationFilePath(),{"--crash-upgrade",temp.path(),stage}); REQUIRE(child.waitForFinished(15000)); REQUIRE(child.exitCode()==44);
        const auto backup=prepareStateUpgrade(temp.path()); REQUIRE(!backup.isEmpty());
        { MetadataStore retry(temp.path()+"/metadata.sqlite",true); CHECK(retry.filePath(7)=="C:/synthetic.txt"); }
        finishStateUpgrade(temp.path()); CHECK(prepareStateUpgrade(temp.path()).isEmpty()); CHECK(QDir(temp.path()+"/backups").entryList(QDir::Dirs|QDir::NoDotAndDotDot).size()==1);
        restoreStateBackup(backup,temp.path()+"/restored"); MetadataStore original(temp.path()+"/restored/metadata.sqlite"); CHECK(original.filePath(7)=="C:/synthetic.txt");
    }
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); const auto args=app.arguments();
    if(args.size()==4 && args[1]=="--crash-upgrade") {
        prepareStateUpgrade(args[2],[&](const QString& stage){if(stage==args[3]) TerminateProcess(GetCurrentProcess(),44);});
        { MetadataStore store(args[2]+"/metadata.sqlite",true); }
        if(args[3]=="migration_committed") TerminateProcess(GetCurrentProcess(),44); return 2;
    }
    return Catch::Session().run(argc,argv);
}
