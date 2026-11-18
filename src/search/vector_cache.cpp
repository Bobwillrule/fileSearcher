#include "search/vector_cache.h"
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QUuid>
#include <stdexcept>
namespace filesearcher {
namespace {
QByteArray digest(const QString& path) {
    QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || file.size()>512LL*1024*1024 || !hash.addData(&file)) throw std::runtime_error("Cannot verify vector cache");
    return hash.result().toHex();
}
}
CachedVectors recoverVectorCache(EmbeddingStore& store,const QString& directory,const std::atomic_bool& cancelled,const std::atomic_bool& disabled) {
    const auto stop=[&] { if (cancelled || disabled) throw std::runtime_error("AI indexing stopped; completed vectors are saved."); };
    stop(); const auto generation=store.generation(),count=store.count();
    if (count>100000) throw std::runtime_error("Vector cache limit reached (100,000 chunks). Saved vectors and keyword search remain available.");
    if (!QDir().mkpath(directory)) throw std::runtime_error("Cannot create vector cache folder");
    const auto manifestPath=directory+"/current.json";
    auto validate=[&](const QString& path) {
        auto index=std::make_unique<VectorIndex>(384,std::max<qint64>(1,count));
        index->load(std::filesystem::path(path.toStdWString()));
        if (index->size()!=count) throw std::runtime_error("Vector cache count mismatch");
        store.eachVector([&](qint64 id,const auto&) { stop(); if (!index->contains(id)) throw std::runtime_error("Vector cache ID mismatch"); });
        if (store.generation()!=generation) throw std::runtime_error("Embedding generation changed during cache validation");
        return index;
    };
    try {
        QFile manifest(manifestPath);
        if (!manifest.open(QIODevice::ReadOnly) || manifest.size()>4096) throw std::runtime_error("Cache manifest unavailable");
        const auto object=QJsonDocument::fromJson(manifest.readAll()).object(); const auto name=object["file"].toString();
        static const QRegularExpression validName("^graph-[0-9a-f-]{36}\\.hnsw$");
        if (object["format"].toInt()!=1 || object["model"].toString()!=store.model() || object["generation"].toString()!=QString::number(generation) ||
            object["count"].toInteger(-1)!=count || !validName.match(name).hasMatch()) throw std::runtime_error("Stale vector cache");
        const auto path=directory+'/'+name;
        if (digest(path)!=object["sha256"].toString().toLatin1()) throw std::runtime_error("Damaged vector cache");
        return {validate(path),false,generation};
    } catch (...) { stop(); }
    auto index=std::make_unique<VectorIndex>(384,std::max<qint64>(1,count));
    store.eachVector([&](qint64 id,const auto& vector) { stop(); index->add(id,vector); });
    const auto name="graph-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".hnsw";
    const auto path=directory+'/'+name,temporary=path+".part";
    try {
        index->save(std::filesystem::path(temporary.toStdWString()));
        index.reset(); index=validate(temporary); stop();
        const auto checksum=digest(temporary);
        if (!QFile::rename(temporary,path)) throw std::runtime_error("Cannot publish vector cache payload");
        QSaveFile manifest(manifestPath);
        const auto bytes=QJsonDocument(QJsonObject{{"format",1},{"model",store.model()},{"generation",QString::number(generation)},
            {"count",count},{"file",name},{"sha256",QString::fromLatin1(checksum)}}).toJson(QJsonDocument::Compact);
        if (store.generation()!=generation || !manifest.open(QIODevice::WriteOnly) || manifest.write(bytes)!=bytes.size() || !manifest.commit())
            throw std::runtime_error("Cannot publish vector cache generation");
        // Only app-owned cache names in this dedicated directory are reclaimed.
        QDir cache(directory);
        for (const auto& old:cache.entryList({"graph-*.hnsw","graph-*.hnsw.part"},QDir::Files)) if (old!=name) cache.remove(old);
    } catch (...) { QFile::remove(temporary); throw; }
    return {std::move(index),true,generation};
}
}
