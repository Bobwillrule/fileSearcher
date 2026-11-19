#include "organization/file_intelligence.h"
#include "search/hybrid_search.h"
#include "ai/model_assets.h"
#include <QSqlError>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QUuid>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace filesearcher {
namespace {
struct Reader {
    QString connection=QUuid::createUuid().toString(); QSqlDatabase db;
    explicit Reader(const QString& database):db(QSqlDatabase::addDatabase("QSQLITE",connection)) {
        db.setDatabaseName(database); db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=2000");
        if(!db.open()) { db={}; QSqlDatabase::removeDatabase(connection); throw std::runtime_error("Search index unavailable"); }
    }
    ~Reader() { db.close(); db={}; QSqlDatabase::removeDatabase(connection); }
    QSqlQuery query(const QString& text,const QVariantList& values={}) {
        QSqlQuery q(db); if(!q.prepare(text)) throw std::runtime_error(q.lastError().text().toStdString());
        for(const auto& value:values) q.addBindValue(value);
        if(!q.exec()) throw std::runtime_error(q.lastError().text().toStdString()); return q;
    }
};
QSet<QString> words(const QString& text) {
    static const QRegularExpression token("[\\p{L}\\p{N}]{3,}",QRegularExpression::UseUnicodePropertiesOption);
    static const QSet<QString> ignored{"the","and","for","with","from","this","that","file","files","document","documents","txt","pdf","docx","notes","about","are","was","into","have","has"};
    QSet<QString> result; auto found=token.globalMatch(text.toCaseFolded());
    while(found.hasNext() && result.size()<256) { auto word=found.next().captured(); if(!ignored.contains(word)) result.insert(word); } return result;
}
QVariantMap row(QSqlQuery& q) { return {{"fileId",q.value(0)},{"path",q.value(1)},{"name",q.value(2)},{"size",q.value(3)},{"modified",q.value(4)}}; }
QVariantMap file(Reader& reader,qint64 id) {
    auto q=reader.query("SELECT id,path,name,size,modified FROM files WHERE id=?",{id});
    if(!q.next()) throw std::runtime_error("Select a file that is still indexed"); auto result=row(q);
    const QFileInfo info(result["path"].toString());
    if(!info.isFile() || info.size()!=result["size"].toLongLong() || info.lastModified().toMSecsSinceEpoch()!=result["modified"].toLongLong())
        throw std::runtime_error("The selected file changed since indexing. Rescan its folder first");
    return result;
}
struct Sample { std::vector<float> vector; QString text; };
QList<Sample> samples(Reader& reader,qint64 file) {
    auto count=reader.query("SELECT count(*) FROM embedding_chunks e JOIN passages p ON p.id=e.passage_id WHERE p.file_id=? AND e.model=? AND e.vector IS NOT NULL",{file,modelFingerprint()}); count.next();
    const int n=count.value(0).toInt(); QSet<int> offsets; if(n) { offsets<<0<<(n/2)<<(n-1); }
    QList<Sample> result;
    auto orderedOffsets=offsets.values(); std::sort(orderedOffsets.begin(),orderedOffsets.end());
    for(int offset:orderedOffsets) {
        auto q=reader.query("SELECT e.vector,substr(p.text,e.start_offset+1,e.length) FROM embedding_chunks e JOIN passages p ON p.id=e.passage_id WHERE p.file_id=? AND e.model=? AND e.vector IS NOT NULL ORDER BY p.ordinal,e.ordinal LIMIT 1 OFFSET ?",{file,modelFingerprint(),offset});
        if(!q.next()) continue; const auto bytes=q.value(0).toByteArray(); if(bytes.size()!=1536) continue;
        std::vector<float> vector(384); double norm=0;
        for(int i=0;i<384;++i) { vector[i]=std::bit_cast<float>(qFromLittleEndian<quint32>(bytes.data()+4*i)); norm+=double(vector[i])*vector[i]; }
        if(std::isfinite(norm) && std::abs(norm-1)<0.001) result<<Sample{std::move(vector),q.value(1).toString()};
    }
    return result;
}
QString project(const QString& path) {
    QDir dir(QFileInfo(path).absolutePath());
    for(;;) { if(dir.exists(".git") || dir.exists("CMakeLists.txt") || dir.exists("package.json")) return dir.absolutePath(); if(!dir.cdUp()) return {}; }
}
void stop(const std::atomic_bool& cancelled) { if(cancelled) throw std::runtime_error("Discovery cancelled"); }
}
QVariantMap organizationFile(const QString& database,qint64 id) { Reader reader(database); return file(reader,id); }
QVariantList relatedFiles(const QString& database,const QString& cache,qint64 id,const std::atomic_bool& cancelled,bool useSemantics) {
    Reader reader(database); const auto source=file(reader,id); const auto passages=useSemantics?samples(reader,id):QList<Sample>{};
    QHash<qint64,QVariantMap> candidates;
    if(!passages.isEmpty()) {
        std::vector<float> centroid(384); double norm=0;
        for(const auto& passage:passages) for(int i=0;i<384;++i) centroid[i]+=passage.vector[i];
        for(float v:centroid) norm+=double(v)*v;
        if(norm>0) {
            for(float& v:centroid) v=static_cast<float>(v/std::sqrt(norm));
            SearchOptions o; o.query=source["name"].toString(); o.limit=200;
            const auto found=retrieveHybrid(database,cache,o,centroid,cancelled);
            for(const auto& value:found.page.rows) { auto r=value.toMap(); candidates.insert(r["fileId"].toLongLong(),r); }
        }
    }
    const auto folder=QFileInfo(source["path"].toString()).absolutePath(),context=project(source["path"].toString());
    auto terms=words(QFileInfo(source["name"].toString()).completeBaseName()); QStringList clauses{"substr(path_key,1,?)=?"};
    const auto prefix=pathKey(context.isEmpty()?folder:context)+'/'; QVariantList values{prefix.size(),prefix};
    auto orderedTerms=terms.values(); std::sort(orderedTerms.begin(),orderedTerms.end());
    for(const auto& term:orderedTerms.mid(0,6)) { clauses<<"instr(name_key,?)>0"; values<<term; }
    auto q=reader.query("SELECT id,path,name,size,modified FROM files WHERE "+clauses.join(" OR ")+" ORDER BY id LIMIT 1000",values);
    while(q.next()) { auto r=row(q); const auto key=r["fileId"].toLongLong(); if(!candidates.contains(key)) candidates.insert(key,r); }
    QList<QVariantMap> ordered;
    for(auto r:candidates) {
        stop(cancelled); if(r["fileId"].toLongLong()==id || pathKey(r["path"].toString())==pathKey(source["path"].toString())) continue;
        const double semantic=r["semanticScore"].toDouble(); const bool sameFolder=pathKey(QFileInfo(r["path"].toString()).absolutePath())==pathKey(folder);
        const bool sameProject=!context.isEmpty() && withinPath(r["path"].toString(),context);
        const auto shared=terms & words(QFileInfo(r["name"].toString()).completeBaseName());
        if(semantic<0.65 && !(shared.size()>=2 || (!shared.isEmpty() && (sameFolder || sameProject)))) continue;
        QStringList reasons; if(semantic>=0.65) reasons<<"Similar indexed passages";
        if(!shared.isEmpty()) { auto list=shared.values(); std::sort(list.begin(),list.end()); reasons<<"Shared filename terms: "+list.join(", "); }
        if(sameFolder) reasons<<"Same folder"; if(sameProject) reasons<<"Same project folder";
        r["reason"]=reasons.join(" · "); r["relationshipScore"]=semantic*0.8+std::min(0.2,shared.size()*0.08)+(sameFolder?0.12:0)+(sameProject?0.1:0);
        ordered<<r;
    }
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b) { const auto x=a["relationshipScore"].toDouble(),y=b["relationshipScore"].toDouble(); return x!=y?x>y:a["fileId"].toLongLong()<b["fileId"].toLongLong(); });
    QVariantList result; for(const auto& r:ordered.mid(0,12)) result<<r; return result;
}
QVariantList categorySuggestions(const QString& database,OrganizationStore& organization,qint64 id,const std::atomic_bool& cancelled,bool useSemantics) {
    Reader reader(database); const auto source=file(reader,id); const auto sourceSamples=useSemantics?samples(reader,id):QList<Sample>{};
    QString text=source["name"].toString(); QStringList evidence;
    auto q=reader.query("SELECT text FROM passages WHERE file_id=? ORDER BY ordinal LIMIT 8",{id});
    while(q.next()) { const auto t=q.value(0).toString(); text+=' '+t; evidence<<t; }
    const auto sourceWords=words(text); QList<QVariantMap> ranked;
    for(const auto& value:organization.categories()) {
        stop(cancelled); const auto category=value.toMap(); auto shared=sourceWords & words(category["name"].toString()+' '+category["description"].toString());
        double score=shared.size()>=2?0.55+std::min(0.24,shared.size()*0.08):0; QString reason,snippet;
        if(score>0) { auto terms=shared.values(); std::sort(terms.begin(),terms.end()); reason="Matching category terms: "+terms.join(", "); for(const auto& e:evidence) if(!(words(e)&shared).isEmpty()) { snippet=e.left(400); break; } }
        for(const auto& example:category["examples"].toList()) {
            if(!useSemantics) break;
            stop(cancelled); if(pathKey(example.toString())==pathKey(source["path"].toString())) continue;
            auto lookup=reader.query("SELECT id FROM files WHERE path_key=?",{pathKey(example.toString())}); if(!lookup.next()) continue;
            const auto fileId=lookup.value(0).toLongLong();
            try { file(reader,fileId); } catch(...) { continue; }
            const auto exampleSamples=samples(reader,fileId);
            for(const auto& a:sourceSamples) for(const auto& b:exampleSamples) {
                double similarity=0; for(int i=0;i<384;++i) similarity+=double(a.vector[i])*b.vector[i];
                if(similarity>=0.72 && similarity>score) { score=similarity; reason="Indexed passage similar to example: "+example.toString(); snippet=a.text.left(400); }
            }
        }
        if(score<0.70) continue;
        auto r=category; r["score"]=score; r["reason"]=reason; r["snippet"]=snippet;
        r["decision"]=organization.decision(category["id"].toLongLong(),source["path"].toString(),source["size"].toLongLong(),source["modified"].toLongLong()); ranked<<r;
    }
    std::sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b) { return a["score"].toDouble()>b["score"].toDouble(); });
    // Ambiguous evidence is an abstention, not an arbitrary winning category.
    if(ranked.isEmpty() || (ranked.size()>1 && ranked[0]["score"].toDouble()-ranked[1]["score"].toDouble()<0.06) || !ranked[0]["decision"].toString().isEmpty()) return {};
    return {ranked.front()};
}
}
