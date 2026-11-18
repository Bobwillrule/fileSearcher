#include "search/hybrid_search.h"
#include "storage/embedding_store.h"
#include "search/vector_cache.h"
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <cmath>
using namespace filesearcher;
namespace {
std::vector<float> vector(float similarity=1) { std::vector<float> v(384); v[0]=similarity; v[1]=std::sqrt(1-similarity*similarity); return v; }
struct Fixture {
    QTemporaryDir directory;
    QString database=directory.path()+"/metadata.sqlite",cache=directory.path()+"/vectors";
    MetadataStore metadata{database,true}; EmbeddingStore embeddings{database};
    qint64 root=metadata.addRoot(directory.path()+"/docs",{}),other=metadata.addRoot(directory.path()+"/other",{});
    std::atomic_bool cancelled{false};
    qint64 add(QString name,QString text,int size=100,qint64 modified=1000,qint64 selected=0,int copies=1,float similarity=1) {
        if (!selected) selected=root;
        const auto path=directory.path()+(selected==root?"/docs/":"/other/")+name;
        metadata.beginScan(); metadata.record(selected,"one",{path,name,QFileInfo(name).suffix(),size,modified}); metadata.finishScan(selected,"one",false);
        const auto pending=metadata.pendingContent(selected); REQUIRE(pending.size()==1);
        QList<Passage> passages; for (int i=0;i<copies;++i) passages<<Passage{text,QString("line %1").arg(i+1),5};
        REQUIRE(metadata.replaceContent(pending.front(),"ok",passages));
        for (;;) {
            auto parts=embeddings.pendingPassages(selected); if (parts.isEmpty()) break;
            for (const auto& p:parts) REQUIRE(embeddings.prepare(p,{{0,static_cast<int>(p.text.size()),5,p.text}}));
        }
        for (;;) {
            auto batch=embeddings.pending(selected,8); if (batch.isEmpty()) break;
            REQUIRE(embeddings.commit(batch,std::vector<std::vector<float>>(batch.size(),vector(similarity)))==batch.size());
        }
        return pending.front().id;
    }
    HybridResult search(SearchOptions options,bool semantic=true) { return retrieveHybrid(database,cache,options,semantic?vector():std::vector<float>{},cancelled); }
};
}
TEST_CASE("Hybrid ranking boosts exact names and groups evidence by file") {
    Fixture f;
    auto exact=f.add("target.txt","An unrelated subject",100,1000,0,1,0);
    f.add("long.md","target topic repeated",100,1000,0,12);
    f.add("short.md","target topic concise");
    SearchOptions o; o.query="target.txt"; auto found=f.search(o); REQUIRE(found.page.rows.size()==3);
    CHECK(found.page.rows[0].toMap()["fileId"].toLongLong()==exact);
    o.query="topic"; found=f.search(o); REQUIRE(found.page.rows.size()==2);
    QSet<qint64> ids;
    for (auto value:found.page.rows) {
        const auto row=value.toMap(); ids.insert(row["fileId"].toLongLong()); CHECK(row["passageId"].toLongLong()>0);
        CHECK(row["matchLabel"].toString().contains("Similar meaning")); CHECK_FALSE(row["snippet"].toString().isEmpty());
        CHECK(f.metadata.preview(row["fileId"].toLongLong(),row["passageId"].toLongLong())["text"].toString().contains("topic"));
    }
    CHECK(ids.size()==found.page.rows.size());
    o.limit=1; CHECK(f.search(o).page.rows.size()==1);
}
TEST_CASE("Every hybrid channel respects root type size and date filters") {
    Fixture f; const auto wanted=f.add("target.txt","target topic",100,2000);
    f.add("target.md","target topic",100,2000); f.add("other.txt","target topic",100,2000,f.other);
    f.add("large.txt","target topic",500,2000); f.add("small.txt","target topic",10,2000); f.add("old.txt","target topic",100,100);
    SearchOptions o; o.query="target"; o.extension=".TXT"; o.root=f.root; o.minimumSize=50; o.maximumSize=200; o.modifiedAfter=1000;
    for (bool semantic:{false,true}) { auto found=f.search(o,semantic); REQUIRE(found.page.rows.size()==1); CHECK(found.page.rows[0].toMap()["fileId"].toLongLong()==wanted); }
    o.extension="pdf"; CHECK(f.search(o).page.rows.isEmpty());
    o.query=""; CHECK(f.search(o).page.rows.isEmpty());
    o.query="\" OR (*)"; CHECK_NOTHROW(f.search(o,false));
    f.cancelled=true; CHECK_THROWS(f.search(o));
}
TEST_CASE("HNSW expands past dense files and stale caches cannot revive deleted evidence") {
    Fixture f; f.add("dense.txt","one repeated topic",100,1000,0,2050);
    std::atomic_bool disabled{false};
    recoverVectorCache(f.embeddings,f.cache,f.cancelled,disabled);
    SearchOptions o; o.query="unseen paraphrase";
    CHECK(f.search(o).semanticPath=="hnsw");
    const auto rare=f.add("rare.md","another relevant topic",200,2000,0,1,0.8F);
    const auto last=f.add("last.txt","third relevant topic",100,1000,0,1,0.7F);
    auto built=recoverVectorCache(f.embeddings,f.cache,f.cancelled,disabled);
    auto found=f.search(o); CHECK(found.semanticPath=="exact-fallback"); REQUIRE(found.page.rows.size()==3);
    CHECK(found.page.rows[1].toMap()["fileId"].toLongLong()==rare); CHECK(found.page.rows[2].toMap()["fileId"].toLongLong()==last);
    o.extension="md"; auto filtered=f.search(o); CHECK(filtered.semanticPath=="exact"); REQUIRE(filtered.page.rows.size()==1);
    CHECK(filtered.page.rows[0].toMap()["fileId"].toLongLong()==rare);
    o.extension=""; REQUIRE(QFile::remove(f.cache+"/current.json")); CHECK(f.search(o).semanticPath=="exact-fallback");
    recoverVectorCache(f.embeddings,f.cache,f.cancelled,disabled);
    f.metadata.resetContent(); CHECK(f.search(o).page.rows.isEmpty());
    CHECK(f.embeddings.count()==0);
}
TEST_CASE("AI absence and low similarities leave honest lexical results") {
    Fixture f; f.add("notes.txt","Master Theorem",100,1000,0,1,0.3F);
    SearchOptions o; o.query="Master";
    auto found=searchHybrid(f.database,f.cache,"missing.exe",f.directory.path(),o,true,f.cancelled);
    CHECK(found.notice.contains("unavailable")); REQUIRE(found.page.rows.size()==1); CHECK_FALSE(found.semantic);
    found=searchHybrid(f.database,f.cache,"missing.exe",f.directory.path(),o,false,f.cancelled); CHECK(found.notice.contains("off"));
    o.query="absent subject"; found=f.search(o); CHECK(found.page.rows.isEmpty()); CHECK(found.notice.contains("No strong"));
    o.query=QString(1025,'a'); CHECK_THROWS(f.search(o));
}
int main(int argc,char** argv) { QCoreApplication app(argc,argv); return Catch::Session().run(argc,argv); }
