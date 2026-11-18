#include "storage/metadata_store.h"
#include "indexing/file_scanner.h"
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QProcess>
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <iostream>
using namespace filesearcher;
namespace {
void write(const QString& path, QByteArray contents = "fixture") {
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path); REQUIRE(file.open(QIODevice::WriteOnly)); REQUIRE(file.write(contents) == contents.size());
}
struct Fixture {
    QTemporaryDir directory;
    QString root = directory.path() + "/documents", database = directory.path() + "/index.sqlite";
    std::atomic_bool cancelled{false};
    Fixture() { REQUIRE(directory.isValid()); REQUIRE(QDir().mkpath(root)); }
    ScanReport scan(MetadataStore& store, qint64 id, const std::function<void(qint64)>& callback = {}) {
        for (const auto& entry : store.roots()) if (entry.id == id) return scanRoot(store, entry, {}, cancelled, callback);
        throw std::runtime_error("Fixture root missing");
    }
};
}
TEST_CASE("Repeated and overlapping scans preserve unique paths across restart") {
    Fixture f;
    write(f.root + "/lecture.pdf"); write(f.root + "/sub/main.cpp"); write(f.root + "/.git/ignored.txt");
    qint64 broad, narrow;
    {
        MetadataStore store(f.database, true);
        broad = store.addRoot(f.root,{".git"}); narrow = store.addRoot(f.root + "/sub",{});
        CHECK(store.addRoot(f.root,{}) == broad);
        CHECK(f.scan(store,broad).files == 2); CHECK(f.scan(store,narrow).files == 1);
        CHECK(f.scan(store,broad).errors == 0); CHECK(store.search({}).total == 2);
        SearchOptions options; options.root = narrow; CHECK(store.search(options).total == 1);
    }
    MetadataStore reopened(f.database);
    REQUIRE(reopened.roots().size() == 2);
    CHECK(reopened.roots()[0].exclusions.contains(".git"));
    CHECK(reopened.search({}).total == 2);
    reopened.removeRoot(broad); CHECK(reopened.search({}).total == 1);
    reopened.removeRoot(narrow); CHECK(reopened.search({}).total == 0);
    CHECK(QFileInfo::exists(f.root + "/lecture.pdf")); CHECK(QFileInfo::exists(f.root + "/sub/main.cpp"));
}
TEST_CASE("Search ranks exact then prefix and treats wildcard characters literally") {
    Fixture f; MetadataStore store(f.database,true); auto root = store.addRoot(f.root,{});
    write(f.root + "/report"); write(f.root + "/report-final.txt"); write(f.root + "/my-report.txt");
    write(f.root + "/100%_done.txt"); write(f.root + "/100XXdone.txt");
    write(f.root + QString::fromUtf8("/Résumé_東京.cpp"));
    auto scan = f.scan(store,root); INFO(scan.message.toStdString()); REQUIRE(scan.errors == 0);
    SearchOptions options; options.query = "report";
    auto result = store.search(options); REQUIRE(result.total == 3);
    CHECK(result.rows[0].toMap()["name"].toString() == "report"); CHECK(result.rows[1].toMap()["name"].toString() == "report-final.txt");
    options.query = "%_"; CHECK(store.search(options).total == 1);
    options.query = "' OR 1=1 --"; CHECK(store.search(options).total == 0);
    options.query = QString::fromUtf8("RÉSUMÉ"); CHECK(store.search(options).total == 1);
    options.query = "documents/100"; CHECK(store.search(options).total == 2);
    options.query = "documents\\100"; CHECK(store.search(options).total == 2);
    options.query = ""; options.extension = ".CPP"; CHECK(store.search(options).total == 1);
    options.extension = ""; options.minimumSize = 8; CHECK(store.search(options).total == 0);
    options.minimumSize = -1; options.maximumSize = 6; CHECK(store.search(options).total == 0);
    options.maximumSize = -1; options.modifiedAfter = QDateTime::currentMSecsSinceEpoch() + 60000; CHECK(store.search(options).total == 0);
}
TEST_CASE("Cancellation rolls back new files and keeps readers responsive") {
    Fixture f; MetadataStore store(f.database,true); auto root = store.addRoot(f.root,{});
    write(f.root + "/original.txt"); REQUIRE(f.scan(store,root).files == 1);
    for (int i = 0; i < 220; ++i) write(f.root + QString("/new-%1.txt").arg(i));
    auto report = f.scan(store,root,[&](qint64 count) {
        if (count >= 100) {
            MetadataStore reader(f.database);
            CHECK(reader.search({}).total == 1);
            f.cancelled.store(true);
        }
    });
    CHECK(report.cancelled); CHECK(store.search({}).total == 1);
    f.cancelled.store(false); REQUIRE(f.scan(store,root).errors == 0); CHECK(store.search({}).total == 221);
}
TEST_CASE("Missing roots and incomplete scans retain previously indexed data") {
    Fixture f; MetadataStore store(f.database,true); auto root = store.addRoot(f.root,{});
    write(f.root + "/original.txt"); REQUIRE(f.scan(store,root).files == 1);
    const auto moved = f.directory.path() + "/temporarily-unavailable";
    REQUIRE(QDir().rename(f.root,moved));
    CHECK(f.scan(store,root).errors > 0); CHECK(store.search({}).total == 1);
    REQUIRE(QDir().rename(moved,f.root));
    REQUIRE(QFile::remove(f.root + "/original.txt"));
    REQUIRE(f.scan(store,root).errors == 0); CHECK(store.search({}).total == 0);
}
TEST_CASE("Locked files and hard links retain distinct searchable paths") {
    Fixture f; MetadataStore store(f.database,true); auto root = store.addRoot(f.root,{});
    const auto source = f.root + "/locked.txt", alias = f.root + "/alias.txt";
    write(source);
    REQUIRE(CreateHardLinkW(alias.toStdWString().c_str(),source.toStdWString().c_str(),nullptr));
    HANDLE handle = CreateFileW(source.toStdWString().c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    auto report = f.scan(store,root);
    CloseHandle(handle);
    CHECK(report.errors == 0); CHECK(store.search({}).total == 2);
}
TEST_CASE("Directory junctions are skipped and cannot be selected as roots") {
    Fixture f; MetadataStore store(f.database,true); auto root = store.addRoot(f.root,{});
    auto external = f.directory.path() + "/external"; write(external + "/outside.txt");
    auto junction = f.root + "/linked";
    const auto result = QProcess::execute("cmd.exe", {"/d","/c","mklink","/J",QDir::toNativeSeparators(junction),QDir::toNativeSeparators(external)});
    REQUIRE(result == 0);
    CHECK_THROWS(validatedRoot(junction));
    auto report = f.scan(store,root);
    CHECK(report.skipped == 1); CHECK(store.search({}).total == 0);
    // Remove just the junction, never its target tree.
    CHECK(RemoveDirectoryW(junction.toStdWString().c_str()));
    CHECK(QFileInfo::exists(external + "/outside.txt"));
}
TEST_CASE("Long paths and exclusions are indexed without following protected data") {
    Fixture f; MetadataStore store(f.database,true);
    QString longDirectory = f.root;
    for (int i = 0; i < 8; ++i) longDirectory += "/a-long-segment-for-unicode-testing";
    write(longDirectory + "/deep.txt"); write(f.root + "/cache/skip.txt"); write(f.root + "/keep.tmp"); write(f.root + "/private/index.sqlite");
    auto root = store.addRoot(f.root,{"cache", "*.tmp"});
    auto report = scanRoot(store,store.roots().front(),{f.root + "/private"},f.cancelled);
    CHECK(report.errors == 0); CHECK(report.files == 1); CHECK(store.search({}).total == 1);
    CHECK(store.search({}).rows[0].toMap()["path"].toString().size() > 260);
}
TEST_CASE("Access denied directories preserve old records without aborting siblings") {
    Fixture f; MetadataStore store(f.database,true); auto root = store.addRoot(f.root,{});
    auto blocked = f.root + "/blocked"; write(blocked + "/old.txt"); write(f.root + "/visible.txt");
    REQUIRE(f.scan(store,root).files == 2);
    const auto path = blocked.toStdWString();
    PACL oldDacl = nullptr; PSECURITY_DESCRIPTOR original = nullptr;
    REQUIRE(GetNamedSecurityInfoW(path.c_str(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&oldDacl,nullptr,&original) == ERROR_SUCCESS);
    PSECURITY_DESCRIPTOR denied = nullptr;
    REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;0x1;;;WD)",SDDL_REVISION_1,&denied,nullptr));
    PACL denyDacl; BOOL present, defaulted;
    GetSecurityDescriptorDacl(denied,&present,&denyDacl,&defaulted);
    REQUIRE(SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,denyDacl,nullptr) == ERROR_SUCCESS);
    auto report = f.scan(store,root);
    const auto restored = SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,oldDacl,nullptr);
    LocalFree(original); LocalFree(denied);
    REQUIRE(restored == ERROR_SUCCESS);
    CHECK(report.errors >= 1); CHECK(store.search({}).total == 2);
}
TEST_CASE("Files disappearing during a scan do not leave stale results after reconciliation") {
    Fixture f; MetadataStore store(f.database,true); auto root = store.addRoot(f.root,{});
    for (int i = 0; i < 210; ++i) write(f.root + QString("/entry-%1.txt").arg(i));
    f.scan(store,root,[&](qint64 count) { if (count == 100) QFile::remove(f.root + "/entry-209.txt"); });
    CHECK(f.scan(store,root).errors == 0); CHECK(store.search({}).total == 209);
}
TEST_CASE("Rescans update metadata and exclusions without recycling removed file IDs") {
    Fixture f; MetadataStore store(f.database,true); auto root = store.addRoot(f.root,{});
    write(f.root + "/old.txt"); REQUIRE(f.scan(store,root).files == 1);
    const auto oldId = store.search({}).rows.front().toMap()["fileId"].toLongLong();
    write(f.root + "/old.txt", "longer fixture contents");
    REQUIRE(f.scan(store,root).errors == 0);
    CHECK(store.search({}).rows.front().toMap()["size"].toLongLong() == 23);
    store.updateExclusions(root,{"*.txt"}); REQUIRE(f.scan(store,root).files == 0);
    CHECK(store.search({}).total == 0); CHECK(QFileInfo::exists(f.root + "/old.txt"));
    store.updateExclusions(root,{}); REQUIRE(f.scan(store,root).files == 1);
    CHECK(store.filePath(oldId).isEmpty());
    CHECK(store.search({}).rows.front().toMap()["fileId"].toLongLong() != oldId);
}
TEST_CASE("Replacing a root during enumeration cannot erase the published snapshot") {
    Fixture f; MetadataStore store(f.database,true); auto root = store.addRoot(f.root,{});
    write(f.root + "/original.txt"); REQUIRE(f.scan(store,root).files == 1);
    for (int i=0;i<220;++i) write(f.root + QString("/extra-%1.txt").arg(i));
    bool replaced = false;
    auto report = f.scan(store,root,[&](qint64 count) {
        if (count >= 100 && !replaced) {
            replaced = true;
            REQUIRE(QDir().rename(f.root,f.root + "-old")); REQUIRE(QDir().mkpath(f.root));
        }
    });
    REQUIRE(replaced); CHECK(report.errors > 0); CHECK(store.search({}).total == 1);
    CHECK(f.scan(store,root).errors == 0); CHECK(store.search({}).total == 0);
}
TEST_CASE("Offline attributes are excluded without reading file contents") {
    Fixture f; MetadataStore store(f.database,true); auto root = store.addRoot(f.root,{});
    const auto file = f.root + "/offline.txt"; write(file);
    REQUIRE(SetFileAttributesW(file.toStdWString().c_str(),FILE_ATTRIBUTE_OFFLINE));
    const auto report = f.scan(store,root);
    REQUIRE(SetFileAttributesW(file.toStdWString().c_str(),FILE_ATTRIBUTE_NORMAL));
    CHECK(report.skipped == 1); CHECK(report.files == 0); CHECK(store.search({}).total == 0);
    CHECK(f.scan(store,root).files == 1);
}
int main(int argc, char* argv[]) { QCoreApplication app(argc,argv); return Catch::Session().run(argc,argv); }
