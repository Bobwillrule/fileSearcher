#include "search/hybrid_search.h"
#include "search/search_store.h"
#include "search/vector_index.h"
#include "ai/embedding_client.h"
#include "ai/model_assets.h"
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace filesearcher {
struct SearchResources { std::unique_ptr<VectorIndex> graph; qint64 generation=-1; };
namespace {
constexpr double semanticFloor=0.60;
constexpr int candidateLimit=200;
void stop(const std::atomic_bool& cancelled) { if (cancelled) throw std::runtime_error("Search cancelled"); }
std::unique_ptr<VectorIndex> loadCache(const QString& directory,qint64 generation) {
    // Queries never publish/delete caches: indexing is their sole writer.
    try {
        QFile manifest(directory+"/current.json");
        if (!manifest.open(QIODevice::ReadOnly) || manifest.size()>4096) return {};
        const auto object=QJsonDocument::fromJson(manifest.readAll()).object();
        const auto name=object["file"].toString();
        static const QRegularExpression validName("^graph-[0-9a-f-]{36}\\.hnsw$");
        const auto count=object["count"].toInteger(-1);
        if (object["format"].toInt()!=1 || object["generation"].toString()!=QString::number(generation) ||
            object["model"].toString()!=modelFingerprint() || count<0 || count>100000 || !validName.match(name).hasMatch()) return {};
        QFile payload(directory+'/'+name); QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!payload.open(QIODevice::ReadOnly) || payload.size()>512LL*1024*1024 || !hash.addData(&payload) || hash.result().toHex()!=object["sha256"].toString().toLatin1()) return {};
        payload.close();
        auto index=std::make_unique<VectorIndex>(384,std::max<qint64>(1,count));
        index->load(std::filesystem::path((directory+'/'+name).toStdWString()));
        if (index->size()!=count) return {};
        return index;
    } catch (...) { return {}; }
}
struct SemanticCandidate { QVariantMap row; double score; };
bool better(const SemanticCandidate& a,const SemanticCandidate& b) {
    if (a.score!=b.score) return a.score>b.score;
    return a.row["chunkId"].toLongLong()<b.row["chunkId"].toLongLong();
}
QVariantList semanticRows(SearchStore& store,const QString& cache,const SearchOptions& options,
        std::span<const float> query,const std::atomic_bool& cancelled,QString& path,SearchResources* resources) {
    if (query.size()!=384) throw std::runtime_error("Invalid query dimensions");
    double norm=0; for (auto f:query) norm+=static_cast<double>(f)*f;
    if (!std::isfinite(norm) || std::abs(norm-1)>0.001) throw std::runtime_error("Invalid query vector");
    const auto [chunks,files]=store.vectorCounts(options);
    const auto target=std::min<qint64>(candidateLimit,files);
    QHash<qint64,SemanticCandidate> best;
    auto accept=[&](SemanticRow source) {
        stop(cancelled); double score=0;
        for (int i=0;i<384;++i) score+=static_cast<double>(query[i])*source.vector[i];
        if (score<semanticFloor) return;
        const auto id=source.row["fileId"].toLongLong(); SemanticCandidate candidate{std::move(source.row),score};
        if (best.contains(id)) { if (better(candidate,best[id])) best[id]=std::move(candidate); return; }
        if (best.size()>=candidateLimit) {
            auto worst=std::min_element(best.begin(),best.end(),[](const auto& a,const auto& b) { return better(b,a); });
            if (!better(candidate,*worst)) return; best.erase(worst);
        }
        best.insert(id,std::move(candidate));
    };
    SearchResources local;
    auto& retained=resources?*resources:local;
    if (retained.generation!=store.generation()) { retained.graph.reset(); retained.generation=store.generation(); }
    if (chunks>2048 && !retained.graph) retained.graph=loadCache(cache,retained.generation);
    auto* index=chunks>2048?retained.graph.get():nullptr;
    stop(cancelled);
    bool exact=!index; path=chunks<=2048?"exact":"exact-fallback";
    if (index) {
        path="hnsw"; size_t take=std::min<size_t>(64,index->size()); QSet<qint64> visited;
        while (take) {
            stop(cancelled); const auto matches=index->search(query,take); QList<qint64> ids;
            for (const auto& match:matches) if (!visited.contains(match.id)) {
                visited.insert(match.id); ids<<static_cast<qint64>(match.id);
                if (ids.size()==64) { store.vectors(options,ids,accept); ids.clear(); }
            }
            if (!ids.isEmpty()) store.vectors(options,ids,accept);
            if (best.size()>=target) break;
            if (take==index->size()) { exact=true; path="exact-fallback"; break; }
            if (take>=8192) { exact=true; path="exact-fallback"; break; }
            take=std::min(index->size(),take*2);
        }
    }
    if (exact) store.vectors(options,{},accept);
    QList<SemanticCandidate> ordered; for (const auto& candidate:best) ordered<<candidate;
    std::sort(ordered.begin(),ordered.end(),better);
    QVariantList result; for (auto& candidate:ordered) { candidate.row["semanticScore"]=candidate.score; result<<candidate.row; }
    return result;
}
SearchPage fuse(const QVariantList& names,const QVariantList& keywords,const QVariantList& semantic,const SearchOptions& options) {
    struct Candidate { QVariantMap row; double score=0; QStringList channels; bool exact=false; };
    QHash<qint64,Candidate> files;
    auto channel=[&](const QVariantList& rows,const QString& label,double weight) {
        QSet<qint64> seen; int rank=0;
        for (const auto& value:rows) {
            auto row=value.toMap(); const auto id=row["fileId"].toLongLong(); if (seen.contains(id)) continue;
            seen.insert(id); ++rank;
            auto& candidate=files[id]; if (candidate.row.isEmpty() || row["passageId"].toLongLong()>0) candidate.row=row;
            candidate.score+=weight/(60+rank); candidate.channels<<label;
            const auto query=QDir::fromNativeSeparators(options.query.trimmed());
            candidate.exact=QString::compare(row["name"].toString(),query,Qt::CaseInsensitive)==0 ||
                QString::compare(QDir::fromNativeSeparators(row["path"].toString()),query,Qt::CaseInsensitive)==0;
        }
    };
    channel(names,"Filename",1); channel(keywords,"Keywords",1); channel(semantic,"Similar meaning",2);
    QList<Candidate> ordered; for (const auto& item:files) ordered<<item;
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b) {
        if (a.exact!=b.exact) return a.exact;
        if (a.score!=b.score) return a.score>b.score;
        return a.row["fileId"].toLongLong()<b.row["fileId"].toLongLong();
    });
    SearchPage page;
    for (auto& item:ordered) {
        item.row["matchLabel"]=item.channels.join(" · "); item.row["rankScore"]=item.score;
        page.rows<<item.row; if (page.rows.size()>=std::clamp(options.limit,1,candidateLimit)) break;
    }
    page.total=page.rows.size(); return page;
}
}
HybridResult retrieveHybrid(const QString& database,const QString& cache,const SearchOptions& options,
        std::span<const float> queryVector,const std::atomic_bool& cancelled,SearchResources* resources) {
    stop(cancelled);
    if (options.query.size()>1024) throw std::runtime_error("Please shorten the search to 1,024 characters or fewer.");
    if (options.query.trimmed().isEmpty()) { MetadataStore store(database); auto browse=options; browse.mode=0; return {store.search(browse),{},false,{}}; }
    SearchStore store(database); const auto version=store.dataVersion(),generation=store.generation();
    auto names=store.names(options),keywords=store.keywords(options); QVariantList semantic;
    HybridResult result;
    if (!queryVector.empty()) {
        try {
            semantic=semanticRows(store,cache,options,queryVector,cancelled,result.semanticPath,resources); result.semantic=true;
            if (semantic.isEmpty()) result.notice="No strong semantic matches; any results below match filenames or keywords.";
        } catch (...) { stop(cancelled); result.notice="The semantic index is unavailable. Showing filename and keyword matches."; }
    }
    stop(cancelled);
    if (store.generation()!=generation || store.dataVersion()!=version) {
        semantic.clear(); names=store.names(options); keywords=store.keywords(options); result.semantic=false;
        result.notice="The index changed during this search. Showing current keyword results; search again for semantic matches.";
    }
    result.page=fuse(names,keywords,semantic,options); return result;
}
HybridResult searchHybrid(const QString& database,const QString& cache,const QString& worker,const QString& model,
        const SearchOptions& options,bool aiEnabled,const std::atomic_bool& cancelled) {
    stop(cancelled); std::vector<float> query; QString notice;
    if (options.query.size()>1024) throw std::runtime_error("Please shorten the search to 1,024 characters or fewer.");
    if (!options.query.trimmed().isEmpty()) {
        if (!aiEnabled) notice="AI is off. Showing filename and keyword matches.";
        else try {
            SearchStore store(database);
            if (!store.hasVectors(options)) notice="No AI passages are ready for these filters. Showing filename and keyword matches.";
            else {
                std::atomic_bool disabled{false}; EmbeddingClient client(worker,model,2,cancelled,disabled);
                query=client.embed({options.query},true).front();
            }
        } catch (...) { stop(cancelled); notice="AI search is unavailable. Showing filename and keyword matches. Check Local AI settings or shorten your query."; }
    }
    auto result=retrieveHybrid(database,cache,options,query,cancelled);
    if (!notice.isEmpty()) result.notice=notice;
    return result;
}
struct SearchSession::Impl {
    QString database,cache,worker,model,stamp;
    std::shared_ptr<std::atomic_bool> current=std::make_shared<std::atomic_bool>(false);
    std::atomic_bool disabled{false};
    std::unique_ptr<EmbeddingClient> client;
    SearchResources resources;
    int starts=0;
    QString modelStamp() const {
        QString value;
        for (const auto& name:{"/onnx/model.onnx","/vocab.txt","/README.md"}) {
            QFileInfo info(model+name);
            value+=QString::number(info.size())+":"+QString::number(info.lastModified().toMSecsSinceEpoch())+";";
        }
        return value;
    }
};
SearchSession::SearchSession(QString database,QString cache,QString worker,QString model):impl_(std::make_unique<Impl>()) {
    impl_->database=std::move(database); impl_->cache=std::move(cache); impl_->worker=std::move(worker); impl_->model=std::move(model);
}
SearchSession::~SearchSession()=default;
void SearchSession::release() { impl_->client.reset(); impl_->resources={}; }
int SearchSession::workerStarts() const { return impl_->starts; }
qint64 SearchSession::workerPid() const { return impl_->client?impl_->client->processId():0; }
HybridResult SearchSession::search(const SearchOptions& options,bool enabled,std::shared_ptr<std::atomic_bool> cancelled) {
    auto& s=*impl_; stop(*cancelled);
    if (options.query.size()>1024) throw std::runtime_error("Please shorten the search to 1,024 characters or fewer.");
    // Keep the old cancellation object alive until the process has been rebound.
    if (s.client) s.client->setCancellation(*cancelled,s.disabled);
    s.current=std::move(cancelled);
    if (!enabled) release();
    if (options.mode!=5) { MetadataStore store(s.database); return {store.search(options),{},false,{}}; }
    std::vector<float> vector; QString notice;
    if (!options.query.trimmed().isEmpty()) {
        if (!enabled) notice="AI is off. Showing filename and keyword matches.";
        else try {
            SearchStore store(s.database);
            if (!store.hasVectors(options)) notice="No AI passages are ready for these filters. Showing filename and keyword matches.";
            else {
                const auto stamp=s.modelStamp();
                if (stamp!=s.stamp) { s.client.reset(); s.stamp=stamp; }
                if (!s.client) { s.client=std::make_unique<EmbeddingClient>(s.worker,s.model,2,*s.current,s.disabled); ++s.starts; }
                vector=s.client->embed({options.query},true).front();
            }
        } catch (...) {
            s.client.reset(); stop(*s.current);
            notice="AI search is unavailable. Showing filename and keyword matches. Retry the search or check Local AI settings.";
        }
    }
    auto result=retrieveHybrid(s.database,s.cache,options,vector,*s.current,&s.resources);
    if (!notice.isEmpty()) result.notice=notice;
    return result;
}

}
