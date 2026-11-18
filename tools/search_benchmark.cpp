#include "search/hybrid_search.h"
#include "search/vector_cache.h"
#include "ai/embedding_client.h"
#include <QCoreApplication>
#include <QSqlQuery>
#include <QSqlError>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <random>
#include <cmath>
#include <algorithm>
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
using namespace filesearcher;
namespace {
void check(QSqlQuery& q) { if (!q.exec()) throw std::runtime_error(q.lastError().text().toStdString()); }
QJsonObject percentiles(QList<double> times) { std::sort(times.begin(),times.end()); return {{"samples",times.size()},{"median_ms",times[times.size()/2]},{"p95_ms",times[static_cast<qsizetype>(std::ceil(times.size()*0.95))-1]}}; }
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); QTextStream out(stdout);
    try {
        const auto args=app.arguments(); if(args.size()!=3) throw std::runtime_error("Usage: search_benchmark model-directory output.json");
        QTemporaryDir temp; const auto database=temp.path()+"/metadata.sqlite",cache=temp.path()+"/vectors";
        { MetadataStore store(database,true); store.addRoot(temp.path()+"/synthetic",{}); }
        std::atomic_bool cancel{false},disabled{false}; auto token=std::make_shared<std::atomic_bool>(false);
        const auto worker=QCoreApplication::applicationDirPath()+"/embedding_worker.exe";
        const QStringList topics{
            "Master Theorem divide and conquer recursion runtime complexity algorithms",
            "Semiconductor diode electrical current bridge rectifier alternating direct voltage",
            "FPGA VGA monitor display pixel timing horizontal vertical synchronization",
            "Google resume software engineering internship education programming projects",
            "Laptop battery receipt invoice payment purchase warranty",
            "C++ mutex atomic threads memory ownership shared data races",
            "SQLite transactions durable database commits write ahead logging",
            "Digital signal processing low pass filter sensor noise cutoff frequency"};
        std::vector<std::vector<float>> bases;
        { EmbeddingClient encoder(worker,args[1],2,cancel,disabled); bases=encoder.embed(topics); }
        QElapsedTimer timer; timer.start();
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","benchmark-writer"); db.setDatabaseName(database);
            if (!db.open() || !db.transaction()) throw std::runtime_error("Cannot prepare benchmark database");
            QSqlQuery model(db); model.prepare("INSERT INTO embedding_models(fingerprint,generation) VALUES(?,100000)"); model.addBindValue(modelFingerprint()); check(model);
            QSqlQuery file(db); file.prepare("INSERT INTO files(id,path,path_key,name,name_key,extension,size,modified) VALUES(?,?,?,?,?,'txt',?,1000)");
            QSqlQuery membership(db); membership.prepare("INSERT INTO root_files(root_id,file_id,generation) VALUES(1,?,'benchmark')");
            QSqlQuery document(db); document.prepare("INSERT INTO documents(file_id,size,modified,status,attempts) VALUES(?,?,1000,'ok',1)");
            QSqlQuery passage(db); passage.prepare("INSERT INTO passages(id,file_id,ordinal,text,location,tokens) VALUES(?,?,?,?,'lines 1-5',32)");
            QSqlQuery chunk(db); chunk.prepare("INSERT INTO embedding_chunks(id,passage_id,model,ordinal,start_offset,length,text_hash,tokens,vector) VALUES(?,?,?,0,0,?,zeroblob(32),32,?)");
            std::mt19937 random(42); std::uniform_real_distribution<float> noise(-0.012F,0.012F);
            qint64 id=0;
            for (int i=1;i<=50000;++i) {
                const auto name=QString("report-%1.txt").arg(i,5,10,QChar('0')); const auto path=temp.path()+"/synthetic/"+name;
                const auto text=topics[i%topics.size()]+". Synthetic performance fixture; not a quality evaluation document.";
                const int copies=i<=49990?1:5001; const qint64 size=text.size()*copies;
                file.bindValue(0,i); file.bindValue(1,path); file.bindValue(2,path.toCaseFolded()); file.bindValue(3,name); file.bindValue(4,name); file.bindValue(5,size); check(file);
                membership.bindValue(0,i); check(membership); document.bindValue(0,i); document.bindValue(1,size); check(document);
                for (int j=0;j<copies;++j) {
                    ++id; passage.bindValue(0,id); passage.bindValue(1,i); passage.bindValue(2,j); passage.bindValue(3,text); check(passage);
                    auto vector=bases[i%bases.size()]; double norm=0;
                    for (auto& f:vector) { f+=noise(random); norm+=double(f)*f; }
                    for (auto& f:vector) f=static_cast<float>(f/std::sqrt(norm));
                    chunk.bindValue(0,id); chunk.bindValue(1,id); chunk.bindValue(2,modelFingerprint()); chunk.bindValue(3,text.size());
                    chunk.bindValue(4,QByteArray(reinterpret_cast<const char*>(vector.data()),1536)); check(chunk);
                }
            }
            if (id!=100000 || !db.commit()) throw std::runtime_error("Invalid benchmark corpus");
        }
        QSqlDatabase::removeDatabase("benchmark-writer");
        const auto prepareMs=timer.elapsed(); out<<"Prepared 50,000 files / 100,000 vectors in "<<prepareMs<<" ms. Building graph…"<<Qt::endl;
        timer.restart();
        { EmbeddingStore store(database); recoverVectorCache(store,cache,cancel,disabled); }
        const auto graphMs=timer.elapsed(); out<<"Graph ready after "<<graphMs<<" ms. Measuring queries…"<<Qt::endl;
        SearchSession session(database,cache,worker,args[1]); SearchOptions options; options.mode=5; options.query="recursion runtime complexity";
        timer.restart(); const auto first=session.search(options,true,token); const double coldMs=timer.nsecsElapsed()/1e6;
        if (first.page.rows.isEmpty()) throw std::runtime_error("Benchmark search returned no results");
        QList<double> names,hybrid,filtered;
        QStringList paths;
        for (int i=0;i<24;++i) {
            options.mode=0; options.query=i%3==0?"report-00008.txt":i%3==1?"report-001":"absent-file";
            timer.restart(); session.search(options,true,token); names<<timer.nsecsElapsed()/1e6;
            options.mode=5; options.query=topics[i%topics.size()];
            timer.restart(); auto result=session.search(options,true,token); hybrid<<timer.nsecsElapsed()/1e6; paths<<result.semanticPath;
            if (result.page.rows.isEmpty() || !result.semantic) throw std::runtime_error("Benchmark semantic lookup failed");
            options.maximumSize=1000;
            timer.restart(); session.search(options,true,token); filtered<<timer.nsecsElapsed()/1e6; options.maximumSize=-1;
        }
        PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb=sizeof(memory); GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory));
        PROCESS_MEMORY_COUNTERS_EX workerMemory{}; workerMemory.cb=sizeof(workerMemory);
        if (auto process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,static_cast<DWORD>(session.workerPid()))) {
            GetProcessMemoryInfo(process,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&workerMemory),sizeof(workerMemory)); CloseHandle(process);
        }
        qint64 cacheBytes=0; for (const auto& entry:QDir(cache).entryInfoList(QDir::Files)) cacheBytes+=entry.size();
        const auto nameStats=percentiles(names),hybridStats=percentiles(hybrid);
        const QJsonObject report{{"files",50000},{"chunks",100000},{"small_files",49990},{"large_documents",10},{"large_document_passages",5001},
            {"vectors","Eight real BGE base embeddings plus deterministic normalized perturbations; retrieval/load benchmark, not embedding throughput or quality"},
            {"filename",nameStats},{"hybrid",hybridStats},{"filtered_hybrid",percentiles(filtered)},{"cold_hybrid_ms",coldMs},
            {"prepare_ms",prepareMs},{"graph_build_ms",graphMs},{"database_bytes",QFileInfo(database).size()},{"graph_bytes",cacheBytes},
            {"parent_working_set_bytes",static_cast<qint64>(memory.WorkingSetSize)},{"parent_peak_working_set_bytes",static_cast<qint64>(memory.PeakWorkingSetSize)},
            {"worker_working_set_bytes",static_cast<qint64>(workerMemory.WorkingSetSize)},{"worker_peak_working_set_bytes",static_cast<qint64>(workerMemory.PeakWorkingSetSize)},
            {"worker_starts",session.workerStarts()},{"hnsw_queries",paths.count("hnsw")},{"exact_fallback_queries",paths.count("exact-fallback")},
            {"filename_target_met",nameStats["p95_ms"].toDouble()<100},{"hybrid_target_met",hybridStats["p95_ms"].toDouble()<500}};
        QFile output(args[2]); if(!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(report).toJson())<0) throw std::runtime_error("Cannot save benchmark report");
        out<<QJsonDocument(report).toJson()<<Qt::endl;
        return session.workerStarts()==1?0:1;
    } catch(const std::exception& e) { out<<e.what()<<Qt::endl; return 1; }
}
