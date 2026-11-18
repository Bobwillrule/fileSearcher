#include "ai/embedding_engine.h"
#include "ai/model_assets.h"
#include "extraction/resource_limits.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <iostream>
namespace {
void reply(const QJsonObject& object) {
    const auto bytes=QJsonDocument(object).toJson(QJsonDocument::Compact);
    std::cout.write(bytes.constData(),bytes.size()); std::cout << '\n' << std::flush;
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        filesearcher::ParserLimits limits(1024ULL*1024*1024);
        if (!limits.ready()) throw std::runtime_error("Cannot apply inference memory limit");
        if (app.arguments().size()!=3) throw std::runtime_error("Invalid worker arguments");
        const auto directory=app.arguments()[1];
        filesearcher::EmbeddingEngine engine(directory,app.arguments()[2].toInt());
        filesearcher::BertTokenizer tokenizer(directory+"/vocab.txt");
        reply({{"ready",filesearcher::modelFingerprint()}});
        for (;;) {
            QByteArray line; char c;
            while (std::cin.get(c) && c!='\n') {
                line+=c; if (line.size()>128*1024) throw std::runtime_error("Request limit exceeded");
            }
            if (line.isEmpty() && !std::cin) break;
            QJsonParseError error;
            const auto document=QJsonDocument::fromJson(line,&error);
            if (error.error!=QJsonParseError::NoError || !document.isObject()) throw std::runtime_error("Invalid request");
            const auto request=document.object(); const auto texts=request["texts"].toArray();
            if (texts.isEmpty() || texts.size()>8) throw std::runtime_error("Invalid batch");
            std::vector<std::vector<int64_t>> batch;
            for (const auto& text:texts) {
                if (!text.isString() || text.toString().size()>4000) throw std::runtime_error("Text limit exceeded");
                batch.push_back(tokenizer.encode(text.toString(),request["query"].toBool(),false));
            }
            QJsonArray vectors;
            for (const auto& vector:engine.embed(batch)) {
                QJsonArray row; for (float value:vector) row.append(value); vectors.append(row);
            }
            reply({{"vectors",vectors}});
        }
        return 0;
    } catch (...) { reply({{"error","Local AI worker unavailable or request failed"}}); return 1; }
}
