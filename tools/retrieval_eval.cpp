#include "storage/metadata_store.h"
#include "indexing/embedding_indexer.h"
#include "search/hybrid_search.h"
#include "ai/embedding_client.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QSet>
#include <QTextStream>
#include <algorithm>
#include <cmath>
using namespace filesearcher;
namespace {
struct Metrics {
    int positive=0,negative=0,exact=0,paraphrase=0,empty=0,exactFirst=0;
    double recall=0,rr=0,paraRr=0;
    void add(const QJsonObject& query,const QStringList& found) {
        QSet<QString> relevant; for (auto id:query["relevant"].toArray()) relevant.insert(id.toString());
        if (relevant.isEmpty()) { ++negative; if (found.isEmpty()) ++empty; return; }
        ++positive; int hits=0,first=0;
        for (int i=0;i<found.size();++i) if (relevant.contains(found[i])) { if (!first) first=i+1; if (i<10) ++hits; }
        const double reciprocal=first?1.0/first:0;
        recall+=static_cast<double>(hits)/relevant.size(); rr+=reciprocal;
        if (query["kind"]=="filename") { ++exact; if (first==1) ++exactFirst; }
        if (query["kind"]=="paraphrase") { ++paraphrase; paraRr+=reciprocal; }
    }
    QJsonObject json() const { return {{"positives",positive},{"recall_at_10",positive?recall/positive:0},{"mrr",positive?rr/positive:0},
        {"exact_top1",exact?double(exactFirst)/exact:1},{"paraphrase_mrr",paraphrase?paraRr/paraphrase:0},
        {"negative_empty_rate",negative?double(empty)/negative:1}}; }
};
QJsonObject timings(QList<double> times) {
    std::sort(times.begin(),times.end());
    if (times.isEmpty()) return {};
    return {{"samples",times.size()},{"median_ms",times[times.size()/2]},{"p95_ms",times[std::max<qsizetype>(0,static_cast<qsizetype>(std::ceil(times.size()*0.95))-1)]}};
}
bool accepted(const Metrics& lexical,const Metrics& hybrid,bool requireAbsolute) {
    const auto l=lexical.json(),h=hybrid.json();
    return hybrid.positive>0 && h["recall_at_10"].toDouble()>=0.90 && h["mrr"].toDouble()>=0.80 && h["exact_top1"].toDouble()==1 &&
        (1-h["paraphrase_mrr"].toDouble())<=0.85*(1-l["paraphrase_mrr"].toDouble())+1e-12 && h["negative_empty_rate"].toDouble()>=0.75 &&
        (!requireAbsolute || h["paraphrase_mrr"].toDouble()-l["paraphrase_mrr"].toDouble()>=0.15);
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); QTextStream out(stdout);
    try {
        const auto args=app.arguments(); if (args.size()!=5) throw std::runtime_error("Usage: retrieval_eval fixture model split output.json");
        const auto split=args[3]; if (split!="dev" && split!="heldout" && split!="all") throw std::runtime_error("Invalid split");
        QFile input(args[1]); if (!input.open(QIODevice::ReadOnly)) throw std::runtime_error("Missing evaluation fixture");
        const auto fixture=QJsonDocument::fromJson(input.readAll()).object();
        QTemporaryDir temporary; const auto database=temporary.path()+"/metadata.sqlite",cache=temporary.path()+"/vectors";
        MetadataStore store(database,true); const auto root=store.addRoot(temporary.path()+"/docs",{});
        QHash<QString,QString> names; QHash<QString,QString> texts;
        store.beginScan();
        for (auto item:fixture["documents"].toArray()) {
            const auto doc=item.toObject(); const auto name=doc["filename"].toString();
            names[name]=doc["id"].toString(); texts[name]=doc["text"].toString();
            store.record(root,"eval",{temporary.path()+"/docs/"+name,name,QFileInfo(name).suffix(),texts[name].size(),1000});
        }
        store.finishScan(root,"eval",true);
        for (const auto& file:store.pendingContent(root)) {
            const auto text=texts[QFileInfo(file.path).fileName()];
            if (!store.replaceContent(file,"ok",{{text,"lines 1-3",100}})) throw std::runtime_error("Cannot prepare fixture text");
        }
        std::atomic_bool cancelled{false},disabled{false};
        const auto worker=QCoreApplication::applicationDirPath()+"/embedding_worker.exe";
        const auto indexed=indexEmbeddings(database,root,worker,args[2],cache,1,cancelled,disabled);
        if (indexed.total!=names.size()) throw std::runtime_error(indexed.status.toStdString());
        EmbeddingClient client(worker,args[2],2,cancelled,disabled);
        SearchSession productionSession(database,cache,worker,args[2]);
        auto productionCancellation=std::make_shared<std::atomic_bool>(false);
        Metrics lexical,hybrid; QHash<QString,Metrics> lexicalSplits,hybridSplits; QJsonArray records;
        QList<double> lexicalTimes,retrievalTimes,sessionTimes,productionTimes;
        auto ids=[&](const SearchPage& page) { QStringList result; for (const auto& row:page.rows) result<<names[row.toMap()["name"].toString()]; return result; };
        for (auto value:fixture["queries"].toArray()) {
            const auto query=value.toObject(); if (split!="all" && query["split"].toString()!=split) continue;
            SearchOptions options; options.query=query["text"].toString(); options.limit=200; options.mode=5;
            QElapsedTimer timer; timer.start(); auto baseline=retrieveHybrid(database,cache,options,{},cancelled);
            lexicalTimes<<timer.nsecsElapsed()/1e6;
            timer.restart(); auto vector=client.embed({options.query},true).front(); const auto embeddingMs=timer.nsecsElapsed()/1e6;
            timer.restart(); auto result=retrieveHybrid(database,cache,options,vector,cancelled); const auto retrievalMs=timer.nsecsElapsed()/1e6;
            retrievalTimes<<retrievalMs; sessionTimes<<embeddingMs+retrievalMs;
            {
                timer.restart(); const auto production=productionSession.search(options,true,productionCancellation);
                productionTimes<<timer.nsecsElapsed()/1e6;
                if (ids(production.page)!=ids(result.page)) throw std::runtime_error("Production and evaluation rankings differ");
            }
            const auto baselineIds=ids(baseline.page),hybridIds=ids(result.page);
            lexical.add(query,baselineIds); hybrid.add(query,hybridIds);
            lexicalSplits[query["split"].toString()].add(query,baselineIds); hybridSplits[query["split"].toString()].add(query,hybridIds);
            records.append(QJsonObject{{"query",query},{"lexical",QJsonArray::fromStringList(baselineIds)},{"hybrid",QJsonArray::fromStringList(hybridIds)},
                {"notice",result.notice},{"retrieval_ms",retrievalMs}});
        }
        const auto l=lexical.json(),h=hybrid.json();
        bool passed=accepted(lexical,hybrid,split!="dev"); QJsonObject partitions;
        for (auto it=hybridSplits.begin();it!=hybridSplits.end();++it) {
            const bool ok=accepted(lexicalSplits[it.key()],it.value(),it.key()=="heldout"); passed=passed && ok;
            const auto originalGain=it.value().json()["paraphrase_mrr"].toDouble()-lexicalSplits[it.key()].json()["paraphrase_mrr"].toDouble();
            partitions[it.key()]=QJsonObject{{"lexical",lexicalSplits[it.key()].json()},{"hybrid",it.value().json()},{"passed",ok},{"original_absolute_gain_met",originalGain>=0.15}};
        }
        const QJsonObject report{{"split",split},{"documents",names.size()},{"queries",records.size()},{"passed",passed},{"lexical",l},{"hybrid",h},
            {"lexical_latency",timings(lexicalTimes)},{"retrieval_only_latency",timings(retrievalTimes)},
            {"persistent_worker_harness_latency",timings(sessionTimes)},{"production_session_latency_including_first_cold_query",timings(productionTimes)},
            {"production_worker_starts",productionSession.workerStarts()},
            {"partitions",partitions},{"original_absolute_gain_met",h["paraphrase_mrr"].toDouble()-l["paraphrase_mrr"].toDouble()>=0.15},{"results",records}};
        QFile output(args[4]); if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(report).toJson())<0) throw std::runtime_error("Cannot write evaluation report");
        out<<QJsonDocument(QJsonObject{{"split",split},{"passed",passed},{"lexical",l},{"hybrid",h},{"production_session_latency_including_first_cold_query",timings(productionTimes)}}).toJson()<<Qt::endl;
        return passed?0:1;
    } catch (const std::exception& error) { out<<error.what()<<Qt::endl; return 1; }
}
