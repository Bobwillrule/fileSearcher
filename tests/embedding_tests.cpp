#include "storage/metadata_store.h"
#include "storage/embedding_store.h"
#include "search/vector_cache.h"
#include "indexing/embedding_indexer.h"
#include "ai/embedding_client.h"
#include "search/hybrid_search.h"
#define NOMINMAX
#include <windows.h>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QThread>
#include <thread>
#include <limits>
using namespace filesearcher;
namespace {
QString model;
void write(const QString& path,const QByteArray& bytes) {
    QFile file(path); REQUIRE(file.open(QIODevice::WriteOnly)); REQUIRE(file.write(bytes)==bytes.size());
}
struct Fixture {
    QTemporaryDir temporary;
    QString database=temporary.path()+"/index.sqlite",cache=temporary.path()+QString::fromUtf8("/cache-東京");
    MetadataStore metadata{database,true};
    qint64 root=metadata.addRoot(temporary.path()+"/documents",{});
    std::atomic_bool cancel{false},disabled{false};
    Fixture() {
        metadata.beginScan(); metadata.record(root,"one",{temporary.path()+"/documents/notes.txt","notes.txt","txt",100,1});
        metadata.finishScan(root,"one",true);
    }
    void content(const QStringList& texts) {
        auto files=metadata.pendingContent(root);
        if (files.isEmpty()) { metadata.resetContent(); files=metadata.pendingContent(root); }
        QList<Passage> passages; for (const auto& text:texts) passages << Passage{text,"lines 1-2",5};
        REQUIRE(metadata.replaceContent(files.front(),"ok",passages));
    }
    QList<PendingEmbedding> prepare(EmbeddingStore& store) {
        for (const auto& passage:store.pendingPassages(root)) REQUIRE(store.prepare(passage,{{0,static_cast<int>(passage.text.size()),5,passage.text}}));
        return store.pending(root,8);
    }
    EmbeddingReport index(int profile=1,const std::function<void(int)>& progress={}) {
        return indexEmbeddings(database,root,QCoreApplication::applicationDirPath()+"/embedding_worker.exe",model,cache,profile,cancel,disabled,progress);
    }
};
std::vector<float> unit(int dimension=0) { std::vector<float> result(384); result[dimension]=1; return result; }
}
TEST_CASE("Embedding revisions persist and reject stale or mismatched writes") {
    Fixture f; f.content({"Master theorem","Diode rectifier"});
    qint64 first=0;
    {
        EmbeddingStore store(f.database); const auto pending=f.prepare(store); REQUIRE(pending.size()==2); first=pending[0].id;
        REQUIRE(store.commit(pending,{unit(),unit(1)})==2); CHECK(store.generation()==2);
        CHECK(store.commit(pending,{unit(),unit(1)})==0); CHECK(store.count()==2);
        EmbeddingStore other(f.database,"different-model"); CHECK(other.commit(pending,{unit(),unit(1)})==0); CHECK(other.count()==0);
    }
    EmbeddingStore reopened(f.database); CHECK(reopened.count()==2); CHECK(reopened.pending(f.root,8).isEmpty()); CHECK(reopened.pendingPassages(f.root).isEmpty());
    f.content({"New revision"}); CHECK(reopened.count()==0); CHECK(reopened.generation()==4);
    auto pending=f.prepare(reopened); REQUIRE(pending.size()==1); CHECK(pending[0].id>first);
    const auto oldPassage=EmbeddingPassage{1,"Master theorem"}; CHECK_FALSE(reopened.prepare(oldPassage,{{0,14,5,"Master theorem"}}));
    f.content({"Third revision"}); CHECK(reopened.commit(pending,{unit()})==0);
    pending=f.prepare(reopened); auto invalid=unit(); invalid[0]=std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS(reopened.commit(pending,{invalid})); CHECK(reopened.count()==0);
    CHECK(reopened.commit(pending,{unit()})==1);
    f.metadata.removeRoot(f.root); CHECK(reopened.count()==0); CHECK(reopened.commit(pending,{unit()})==0);
}
TEST_CASE("Vector cache restores Unicode paths and recovers deletion corruption and model mismatch") {
    Fixture f; f.content({"Algorithms","Electronics"}); EmbeddingStore store(f.database);
    const auto pending=f.prepare(store); REQUIRE(store.commit(pending,{unit(),unit(1)})==2);
    auto cache=recoverVectorCache(store,f.cache,f.cancel,f.disabled); CHECK(cache.rebuilt);
    CHECK(cache.index->search(unit(),1).front().id==pending[0].id);
    cache=recoverVectorCache(store,f.cache,f.cancel,f.disabled); CHECK_FALSE(cache.rebuilt);
    QFile manifest(f.cache+"/current.json"); REQUIRE(manifest.open(QIODevice::ReadOnly));
    const auto object=QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
    const auto payload=f.cache+'/'+object["file"].toString(); REQUIRE(QFile::remove(payload));
    cache=recoverVectorCache(store,f.cache,f.cancel,f.disabled); CHECK(cache.rebuilt); CHECK(cache.index->size()==2);
    REQUIRE(manifest.open(QIODevice::ReadOnly)); const auto newObject=QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
    write(f.cache+'/'+newObject["file"].toString(),"corrupt");
    CHECK(recoverVectorCache(store,f.cache,f.cancel,f.disabled).rebuilt);
    write(f.cache+"/current.json","{\"file\":\"../unrelated\"}"); CHECK(recoverVectorCache(store,f.cache,f.cancel,f.disabled).rebuilt);
    EmbeddingStore other(f.database,"other-model"); auto empty=recoverVectorCache(other,f.cache,f.cancel,f.disabled); CHECK(empty.rebuilt); CHECK(empty.index->size()==0);
    CHECK(recoverVectorCache(store,f.cache,f.cancel,f.disabled).rebuilt);
    f.metadata.resetContent(); CHECK(store.generation()!=cache.generation);
    cache=recoverVectorCache(store,f.cache,f.cancel,f.disabled); CHECK(cache.index->size()==0); CHECK(cache.rebuilt);
    f.cancel=true; CHECK_THROWS(recoverVectorCache(store,f.cache,f.cancel,f.disabled));
}
TEST_CASE("Missing models do not damage lexical search or pending embeddings") {
    Fixture f; f.content({"Master Theorem explains recurrence relations"});
    const auto report=indexEmbeddings(f.database,f.root,"missing.exe",f.temporary.path(),f.cache,1,f.cancel,f.disabled);
    CHECK(report.embedded==0); CHECK(report.status.contains("checksum"));
    REQUIRE(QDir().mkpath(f.temporary.path()+"/onnx"));
    write(f.temporary.path()+"/onnx/model.onnx","damaged model");
    write(f.temporary.path()+"/vocab.txt","damaged vocabulary"); write(f.temporary.path()+"/README.md","damaged card");
    CHECK_THROWS(verifyModelAssets(f.temporary.path()));
    SearchOptions options; options.query="recurrence"; options.mode=2; CHECK(f.metadata.search(options).total==1);
    EmbeddingStore store(f.database); CHECK(store.pendingPassages(f.root).size()==1);
}
TEST_CASE("Inference startup cancellation stops a nonresponsive child") {
    std::atomic_bool cancel{false},disabled{false};
    std::thread stop([&] { QThread::msleep(100); cancel=true; });
    QElapsedTimer timer; timer.start();
    CHECK_THROWS(EmbeddingClient(QCoreApplication::applicationDirPath()+"/probe_fixture_worker.exe","hang",1,cancel,disabled));
    stop.join(); CHECK(timer.elapsed()<3000);
}
TEST_CASE("Exact model partition never truncates Unicode content", "[.model]") {
    BertTokenizer tokenizer(model+"/vocab.txt");
    const auto text=QString::fromUtf8("東京 recurrence electromagnetic 🙂 ").repeated(180);
    const auto parts=embeddingParts(text,tokenizer); REQUIRE(parts.size()>1);
    QString reconstructed; int offset=0;
    for (const auto& part:parts) {
        CHECK(part.offset==offset); CHECK(part.tokens<=386); CHECK(part.tokens==tokenizer.encode(part.text,false,false).size());
        CHECK_FALSE(part.text.front().isLowSurrogate()); CHECK_FALSE(part.text.back().isHighSurrogate());
        reconstructed+=part.text; offset+=part.length;
    }
    CHECK(reconstructed==text);
}
TEST_CASE("Real local inference resumes batches and reuses saved vectors after restart", "[.model]") {
    Fixture f; QStringList texts;
    for (int i=0;i<12;++i) texts << QString("Lecture %1: Master Theorem solves divide and conquer recurrences.").arg(i);
    f.content(texts);
    auto interrupted=f.index(0,[&](int count) { if (count>=1) f.disabled=true; });
    INFO(interrupted.status.toStdString()); REQUIRE(interrupted.cancelled); CHECK(interrupted.embedded==1);
    { EmbeddingStore store(f.database); CHECK(store.count()==1); }
    f.disabled=false;
    const auto resumed=f.index(2); INFO(resumed.status.toStdString()); REQUIRE(resumed.total==12); CHECK(resumed.embedded==11);
    const auto unchanged=f.index(); INFO(unchanged.status.toStdString()); CHECK(unchanged.embedded==0); CHECK_FALSE(unchanged.rebuilt); CHECK(unchanged.total==12);
    QFile manifest(f.cache+"/current.json"); REQUIRE(manifest.open(QIODevice::ReadOnly)); const auto object=QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
    REQUIRE(QFile::remove(f.cache+'/'+object["file"].toString()));
    const auto recovered=f.index(); CHECK(recovered.embedded==0); CHECK(recovered.rebuilt); CHECK(recovered.total==12);
    EmbeddingStore store(f.database); auto cache=recoverVectorCache(store,f.cache,f.cancel,f.disabled);
    EmbeddingClient client(QCoreApplication::applicationDirPath()+"/embedding_worker.exe",model,1,f.cancel,f.disabled);
    const auto query=client.embed({"How do I solve divide and conquer recurrences?"},true);
    const auto matches=cache.index->search(query[0],3); CHECK(matches.size()==3); CHECK(matches.front().cosine_similarity>0.5);
    CHECK_THROWS(client.embed({QString("word ").repeated(600)},true)); // Reject oversize input instead of truncating it.
    f.content({"Replacement document about silicon diodes and electrical rectification."});
    const auto changed=f.index(); CHECK(changed.embedded==1); CHECK(changed.total==1); CHECK(changed.rebuilt);
    const auto failed=indexEmbeddings(f.database,f.root,"missing-worker.exe",model,f.cache,1,f.cancel,f.disabled);
    CHECK(failed.embedded==0); CHECK(failed.total==1); // No worker is needed for unchanged text.
    f.content({"New text requiring an unavailable worker"});
    const auto unavailable=indexEmbeddings(f.database,f.root,"missing-worker.exe",model,f.cache,1,f.cancel,f.disabled);
    CHECK(unavailable.status.contains("unavailable")); CHECK(store.pending(f.root,8).size()==1);
}
TEST_CASE("Persistent queries reuse inference and recover after process failure", "[.model]") {
    Fixture f; f.content({"The Master Theorem solves divide and conquer recurrence relations."}); REQUIRE(f.index().total==1);
    SearchSession session(f.database,f.cache,QCoreApplication::applicationDirPath()+"/embedding_worker.exe",model);
    SearchOptions options; options.mode=5; options.query="divide conquer recurrences";
    auto token=std::make_shared<std::atomic_bool>(false);
    REQUIRE(session.search(options,true,token).semantic); const auto first=session.workerPid(); REQUIRE(first>0);
    options.query="Master Theorem"; REQUIRE(session.search(options,true,std::make_shared<std::atomic_bool>(false)).semantic);
    CHECK(session.workerPid()==first); CHECK(session.workerStarts()==1);
    auto process=OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE,FALSE,static_cast<DWORD>(first)); REQUIRE(process!=nullptr);
    REQUIRE(TerminateProcess(process,1)); WaitForSingleObject(process,3000); CloseHandle(process);
    const auto failed=session.search(options,true,token); CHECK_FALSE(failed.semantic); CHECK(failed.page.total==1);
    REQUIRE(session.search(options,true,token).semantic); CHECK(session.workerStarts()==2);
    session.release(); CHECK(session.workerPid()==0);
    REQUIRE(session.search(options,true,token).semantic); CHECK(session.workerStarts()==3);
    CHECK_FALSE(session.search(options,false,token).semantic); CHECK(session.workerPid()==0);
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    if (app.arguments().size()==3 && app.arguments()[1]=="--model") {
        model=app.arguments()[2]; const char* args[]={argv[0],"[.model]"}; return Catch::Session().run(2,args);
    }
    return Catch::Session().run(argc,argv);
}
