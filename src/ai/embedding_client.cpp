#include "ai/embedding_client.h"
#include "ai/model_assets.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <cmath>
#include <stdexcept>
namespace filesearcher {
EmbeddingClient::EmbeddingClient(const QString& executable,const QString& model,int threads,
        const std::atomic_bool& cancelled,const std::atomic_bool& disabled):cancelled_(&cancelled),disabled_(&disabled) {
    process_.setProgram(executable); process_.setArguments({model,QString::number(threads)});
    process_.start();
    if (!process_.waitForStarted(3000)) throw std::runtime_error("Local AI worker is unavailable. Keyword search remains available.");
    try {
        if (read()["ready"].toString()!=modelFingerprint()) throw std::runtime_error("Local AI model could not be loaded or verified.");
    } catch (...) { process_.kill(); process_.waitForFinished(2000); throw; }
}
EmbeddingClient::~EmbeddingClient() { process_.kill(); process_.waitForFinished(2000); }
void EmbeddingClient::setCancellation(const std::atomic_bool& cancelled,const std::atomic_bool& disabled) { cancelled_=&cancelled; disabled_=&disabled; }
QJsonObject EmbeddingClient::read() {
    QElapsedTimer timer; timer.start();
    for (;;) {
        if (cancelled_->load() || disabled_->load()) throw std::runtime_error("AI work cancelled.");
        pending_+=process_.readAllStandardOutput(); process_.readAllStandardError();
        if (pending_.size()>256*1024) throw std::runtime_error("AI response exceeded limit");
        const auto newline=pending_.indexOf('\n');
        if (newline>=0) {
            QJsonParseError error;
            const auto document=QJsonDocument::fromJson(pending_.left(newline),&error); pending_.remove(0,newline+1);
            if (error.error!=QJsonParseError::NoError || !document.isObject() || document.object().contains("error"))
                throw std::runtime_error("Local AI worker failed. Keyword search remains available.");
            return document.object();
        }
        if (timer.elapsed()>60000 || process_.state()==QProcess::NotRunning)
            throw std::runtime_error("Local AI worker stopped or timed out. Keyword search remains available.");
        process_.waitForReadyRead(20);
    }
}
std::vector<std::vector<float>> EmbeddingClient::embed(const QStringList& texts,bool query) {
    if (texts.isEmpty() || texts.size()>8) throw std::runtime_error("Invalid embedding batch");
    const auto request=QJsonDocument(QJsonObject{{"texts",QJsonArray::fromStringList(texts)},{"query",query}}).toJson(QJsonDocument::Compact)+'\n';
    if (request.size()>128*1024 || process_.write(request)!=request.size()) throw std::runtime_error("Cannot send embedding request");
    const auto rows=read()["vectors"].toArray();
    if (rows.size()!=texts.size()) throw std::runtime_error("Invalid AI batch response");
    std::vector<std::vector<float>> result;
    for (const auto& row:rows) {
        const auto values=row.toArray(); if (values.size()!=384) throw std::runtime_error("Invalid AI dimensions");
        std::vector<float> vector; double norm=0;
        for (const auto& value:values) {
            if (!value.isDouble() || !std::isfinite(value.toDouble())) throw std::runtime_error("Invalid AI value");
            vector.push_back(static_cast<float>(value.toDouble())); norm+=value.toDouble()*value.toDouble();
        }
        if (std::abs(norm-1)>0.001) throw std::runtime_error("AI vector was not normalized");
        result.push_back(std::move(vector));
    }
    return result;
}
}
