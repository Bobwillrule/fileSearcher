#include "storage/metadata_store.h"
#include "indexing/file_scanner.h"
#include "indexing/content_indexer.h"
#include "extraction/probe_runner.h"
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QSqlQuery>
#include <QStringEncoder>
#include <QThread>
#include <thread>
#include <QProcess>
#define NOMINMAX
#include <windows.h>
using namespace filesearcher;
namespace {
QString fixtures;
QString worker() { return QCoreApplication::applicationDirPath()+"/document_worker.exe"; }
void write(const QString& path,const QByteArray& bytes) {
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath())); QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly)); REQUIRE(file.write(bytes) == bytes.size());
}
struct Fixture {
    QTemporaryDir directory;
    QString root = directory.path()+"/docs", database = directory.path()+"/index.sqlite";
    MetadataStore store{database,true};
    std::atomic_bool cancelled{false};
    qint64 id;
    Fixture() { REQUIRE(directory.isValid()); REQUIRE(QDir().mkpath(root)); id=store.addRoot(root,{}); }
    void scan() { auto report=scanRoot(store,store.roots().front(),{},cancelled); INFO(report.message.toStdString()); REQUIRE(report.errors == 0); }
    SearchPage search(QString query,int mode=2) { SearchOptions options; options.query=query; options.mode=mode; return store.search(options); }
    ContentReport index() { return indexContents(store,id,worker(),cancelled); }
};
}
TEST_CASE("Text worker decodes Unicode encodings and rejects invalid or binary input") {
    QTemporaryDir fixture; const auto text=QString::fromUtf8("Résumé 東京\r\nMaster Theorem");
    for (auto encoding : {QStringConverter::Utf8,QStringConverter::Utf16LE,QStringConverter::Utf16BE,QStringConverter::Utf32LE}) {
        QStringEncoder encoder(encoding,QStringConverter::Flag::WriteBom);
        const auto path=fixture.path()+QString("/encoding-%1.txt").arg(static_cast<int>(encoding));
        write(path,encoder(text)); auto result=runExtractionProbe(worker(),path);
        INFO(result.status.toStdString()); REQUIRE(result.status == "ok");
        CHECK(result.text == QString(text).replace("\r\n","\n"));
    }
    write(fixture.path()+"/binary.txt",QByteArray("ab\0cd",5));
    CHECK(runExtractionProbe(worker(),fixture.path()+"/binary.txt").status == "binary_data");
    write(fixture.path()+"/invalid.txt",QByteArray::fromHex("ff6162"));
    CHECK(runExtractionProbe(worker(),fixture.path()+"/invalid.txt").status == "unsupported_encoding");
    write(fixture.path()+"/incomplete.txt",QByteArray::fromHex("e282"));
    CHECK(runExtractionProbe(worker(),fixture.path()+"/incomplete.txt").status == "unsupported_encoding");
}
TEST_CASE("Worker reports limits and preserves explicitly truncated text") {
    QTemporaryDir fixture;
    write(fixture.path()+"/large.txt",QByteArray(4*1024*1024+1,'x'));
    CHECK(runExtractionProbe(worker(),fixture.path()+"/large.txt").status == "file_too_large");
    write(fixture.path()+"/truncated.md",QByteArray(1000100,'a'));
    const auto result=runExtractionProbe(worker(),fixture.path()+"/truncated.md");
    CHECK(result.status == "text_limit"); CHECK(result.text.size() == 1000000);
    std::atomic_bool cancel{true};
    CHECK(runExtractionProbe(worker(),fixture.path()+"/truncated.md",5000,&cancel).status == "cancelled");
    cancel=false;
    std::thread cancellation([&] { QThread::msleep(80); cancel=true; });
    auto interrupted=runExtractionProbe(QCoreApplication::applicationDirPath()+"/probe_fixture_worker.exe","hang",5000,&cancel);
    cancellation.join(); CHECK(interrupted.status == "cancelled");
}
TEST_CASE("Passages retain page or line locations and overlap without breaking Unicode") {
    QString words; for(int i=0;i<800;++i) words += QString("token%1 ").arg(i);
    auto passages=splitPassages({{words,3,{}}}); REQUIRE(passages.size() >= 3);
    CHECK(passages.front().location == "Page 3");
    CHECK(passages[0].text.contains("token349")); CHECK(passages[1].text.startsWith("token310"));
    for(const auto& passage:passages) { CHECK(passage.tokens <= 350); CHECK(passage.text.size() <= 4000); }
    auto lines=splitPassages({{"first\nsecond\nthird",0,"Body"}});
    CHECK(lines.front().location == QString::fromUtf8("Body: lines 1–3"));
    QString unicode; for(int i=0;i<1000;++i) unicode += QString::fromUtf8("🧪");
    for(const auto& passage:splitPassages({{unicode,0,{}}})) CHECK(QString::fromUtf8(passage.text.toUtf8()) == passage.text);
}
TEST_CASE("Keyword search ranks passages deduplicates files and keeps queries literal") {
    Fixture f;
    write(f.root+"/notes.md","The Master Theorem solves recurrence relations.\n<script>alert('x')</script>\nAND OR NOT are words.");
    write(f.root+"/other.txt","Theorem background with Master separated by several unrelated words.");
    write(f.root+"/master theorem name.bin","unsupported");
    f.scan(); auto report=f.index(); CHECK(report.processed == 3);
    auto terms=f.search("Master Theorem"); REQUIRE(terms.total == 2);
    auto phrase=f.search("Master Theorem",3); REQUIRE(phrase.total == 1);
    CHECK(phrase.rows.front().toMap()["name"].toString() == "notes.md");
    CHECK(phrase.rows.front().toMap()["snippet"].toString().contains("Master"));
    CHECK(f.search("AND OR NOT").total == 1);
    CHECK(f.search("missing OR Theorem").total == 0);
    CHECK(f.search("missing OR Theorem",4).total == 2);
    CHECK(f.search("Master Theorem",1).total == 3);
    CHECK_THROWS(f.search("\"",4)); CHECK_NOTHROW(f.search("\""));
    const auto row=phrase.rows.front().toMap();
    auto preview=f.store.preview(row["fileId"].toLongLong(),row["passageId"].toLongLong());
    CHECK(preview["text"].toString().contains("<script>")); CHECK(preview["count"].toInt() == 1);
    SearchOptions options; options.mode=2; options.query="Master"; options.extension="md"; options.root=f.id;
    CHECK(f.store.search(options).total == 1); options.minimumSize=1000; CHECK(f.store.search(options).total == 0);
    CHECK(f.index().processed == 0);
}
TEST_CASE("New revisions invalidate old passages and stale extraction cannot overwrite them") {
    Fixture f; write(f.root+"/notes.txt","obsoleteunique"); f.scan();
    auto original=f.store.pendingContent(f.id).front(); REQUIRE(f.index().processed == 1);
    CHECK(f.search("obsoleteunique").total == 1);
    write(f.root+"/notes.txt","replacementunique with more text"); f.scan();
    CHECK(f.search("obsoleteunique").total == 0);
    CHECK_FALSE(f.store.replaceContent(original,"ok",{{"obsoleteunique","lines 1–1",1}}));
    REQUIRE(f.index().processed == 1); CHECK(f.search("replacementunique").total == 1);
    // A failed newer revision must not retain old searchable text.
    write(f.root+"/notes.txt",QByteArray::fromHex("ff00")); f.scan(); f.index();
    CHECK(f.search("replacementunique").total == 0);
    f.store.removeRoot(f.id); CHECK(f.search("replacementunique").total == 0);
    CHECK(QFileInfo::exists(f.root+"/notes.txt"));
}
TEST_CASE("PDF and DOCX passages are indexed while malformed and scanned files show notices") {
    Fixture f;
    for(const auto& name:{"text.pdf","notes.docx","scanned.pdf","encrypted.pdf","malformed.docx"}) REQUIRE(QFile::copy(fixtures+'/'+name,f.root+'/'+name));
    f.scan(); auto report=f.index(); CHECK(report.processed == 5); CHECK(report.warnings == 3);
    auto pdf=f.search("Master Theorem",3); REQUIRE(pdf.total == 1);
    CHECK(pdf.rows.front().toMap()["location"].toString() == "Page 1");
    CHECK(f.search("Dynamic programming",3).total == 1);
    auto rows=f.store.search({}).rows;
    for(const auto& value:rows) {
        const auto row=value.toMap();
        if(row["name"].toString() == "scanned.pdf") CHECK(row["contentStatus"].toString() == "no_text_or_ocr_required");
        if(row["name"].toString() == "encrypted.pdf") CHECK(row["contentStatus"].toString() == "password_protected");
    }
    MetadataStore reader(f.database); SearchOptions query; query.query="Master Theorem"; query.mode=3;
    CHECK(reader.search(query).total == 1);
}
TEST_CASE("Content publication is atomic and root deletion cleans FTS records") {
    Fixture f; write(f.root+"/notes.txt","atomicunique"); f.scan(); f.index();
    auto original=f.store.search({}).rows.front().toMap()["fileId"].toLongLong();
    f.store.beginScan(); f.store.record(f.id,"next",{f.root+"/notes.txt","notes.txt","txt",100,1});
    MetadataStore reader(f.database); SearchOptions query; query.mode=2; query.query="atomicunique";
    CHECK(reader.search(query).total == 1); f.store.rollback(); CHECK(f.search("atomicunique").total == 1);
    f.store.resetContent(); CHECK(f.search("atomicunique").total == 0); CHECK(f.store.preview(original)["status"].toString() == "pending");
    f.index(); f.store.removeRoot(f.id); CHECK(f.search("atomicunique").total == 0);
}
TEST_CASE("Version one metadata migrates to content storage without losing roots or files") {
    QTemporaryDir fixture; const auto database=fixture.path()+"/old.sqlite";
    {
        auto db=QSqlDatabase::addDatabase("QSQLITE","migration-fixture"); db.setDatabaseName(database); REQUIRE(db.open());
        {
            QSqlQuery query(db);
            REQUIRE(query.exec("CREATE TABLE files(id INTEGER PRIMARY KEY AUTOINCREMENT,path TEXT NOT NULL,path_key TEXT NOT NULL UNIQUE,name TEXT NOT NULL,name_key TEXT NOT NULL,extension TEXT NOT NULL,size INTEGER NOT NULL,modified INTEGER NOT NULL)"));
            REQUIRE(query.exec("INSERT INTO files VALUES(7,'C:/fixture.txt','c:/fixture.txt','fixture.txt','fixture.txt','txt',8,123)"));
            REQUIRE(query.exec("PRAGMA user_version=1"));
        }
        db.close();
    }
    QSqlDatabase::removeDatabase("migration-fixture");
    MetadataStore store(database,true);
    CHECK(store.filePath(7)=="C:/fixture.txt"); CHECK(store.search({}).total==1);
    ContentFile file{7,"C:/fixture.txt",8,123,0};
    REQUIRE(store.replaceContent(file,"ok",{{"migratedunique","lines 1–1",1}}));
    SearchOptions options; options.mode=2; options.query="migratedunique";
    CHECK(store.search(options).total==1);
}
TEST_CASE("Content BM25 ranks the strongest passage and returns each file once") {
    Fixture f;
    write(f.root+"/best.txt","signal signal signal");
    QByteArray longText="signal "; for(int i=0;i<700;++i) longText+="filler "; longText+="signal";
    write(f.root+"/long.txt",longText); f.scan(); f.index();
    auto result=f.search("signal"); REQUIRE(result.total==2);
    CHECK(result.rows.front().toMap()["name"].toString()=="best.txt");
    QString numbered; for(int i=0;i<800;++i) numbered+=QString("token%1 ").arg(i);
    write(f.root+"/boundary.txt",numbered.toUtf8()); f.scan(); f.index();
    CHECK(f.search("token348 token349 token350",3).total==1);
}
TEST_CASE("The worker rejects a linked parent before reading its contents") {
    QTemporaryDir fixture; const auto target=fixture.path()+"/outside", link=fixture.path()+"/linked";
    write(target+"/notes.txt","This file must not be read through a junction.");
    REQUIRE(QProcess::execute("cmd.exe",{"/d","/c","mklink","/J",QDir::toNativeSeparators(link),QDir::toNativeSeparators(target)})==0);
    auto result=runExtractionProbe(worker(),link+"/notes.txt");
    CHECK(RemoveDirectoryW(link.toStdWString().c_str()));
    CHECK(result.status=="placeholder_or_link"); CHECK(result.text.isEmpty());
}
int main(int argc,char* argv[]) {
    QCoreApplication app(argc,argv); if(argc<2) return 2; fixtures=QString::fromLocal8Bit(argv[1]);
    return Catch::Session().run(1,argv);
}
