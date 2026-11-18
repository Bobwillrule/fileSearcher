#include "ai/bert_tokenizer.h"
#include "search/vector_index.h"
#include "ai/embedding_engine.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QCryptographicHash>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().size() != 3) throw std::runtime_error("Usage: embedding_probe <model-directory> <asset-lock.json>");
        const auto root = app.arguments()[1];
        QFile lockFile(app.arguments()[2]);
        if (!lockFile.open(QIODevice::ReadOnly)) throw std::runtime_error("Asset lock unavailable");
        const auto hashes = QJsonDocument::fromJson(lockFile.readAll()).object()["sha256"].toObject();
        if (hashes.size() != 8) throw std::runtime_error("Invalid asset lock");
        for (auto it = hashes.begin(); it != hashes.end(); ++it) {
            QFile asset(root + '/' + it.key());
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (!asset.open(QIODevice::ReadOnly) || !hash.addData(&asset) || hash.result().toHex() != it.value().toString().toLatin1())
                throw std::runtime_error(("Asset checksum mismatch: " + it.key()).toStdString());
        }
        filesearcher::BertTokenizer tokenizer(root + "/vocab.txt");
        QFile reference(root + "/reference.json");
        if (!reference.open(QIODevice::ReadOnly)) throw std::runtime_error("Reference file unavailable");
        const auto cases = QJsonDocument::fromJson(reference.readAll()).object()["cases"].toArray();
        if (cases.isEmpty()) throw std::runtime_error("Empty reference cases");
        filesearcher::EmbeddingEngine engine(root,4);
        std::vector<std::vector<int64_t>> tokens;
        for (const auto& entry : cases) {
            const auto c = entry.toObject();
            auto ids = tokenizer.encode(c["text"].toString(), c["query"].toBool());
            const auto expected = c["ids"].toArray();
            if (ids.size() != static_cast<size_t>(expected.size())) throw std::runtime_error(("Token count mismatch: " + c["id"].toString()).toStdString());
            for (qsizetype j = 0; j < expected.size(); ++j)
                if (ids[j] != expected[j].toInteger()) throw std::runtime_error(("Token mismatch: " + c["id"].toString() + " at " + QString::number(j)).toStdString());
            tokens.push_back(std::move(ids));
        }
        auto embed = [&](const std::vector<std::vector<int64_t>>& batch) { return engine.embed(batch); };
        std::vector<std::vector<float>> embeddings;
        double max_error = 0;
        for (size_t begin = 0; begin < tokens.size(); begin += 4) {
            const auto end = std::min(begin + 4, tokens.size());
            auto batch = embed({tokens.begin() + begin, tokens.begin() + end});
            for (size_t i = begin; i < end; ++i) {
                auto single = embed({tokens[i]})[0];
                const auto expected = cases[static_cast<qsizetype>(i)].toObject()["embedding"].toArray();
                if (expected.size() != 384) throw std::runtime_error("Invalid reference dimension");
                for (size_t j = 0; j < 384; ++j) {
                    max_error = std::max(max_error, std::abs(batch[i - begin][j] - expected[j].toDouble()));
                    if (std::abs(single[j] - batch[i - begin][j]) > 1e-4) throw std::runtime_error("Batch padding changed embedding");
                }
                embeddings.push_back(std::move(batch[i - begin]));
            }
        }
        if (max_error > 1e-4) throw std::runtime_error("Embedding reference tolerance exceeded");
        filesearcher::VectorIndex index(384, embeddings.size());
        for (int i = 0; i < 5; ++i) index.add(i + 100, embeddings[i]);
        int correct = 0;
        for (int i = 5; i < 10; ++i) {
            auto match = index.search(embeddings[i], 1).front();
            if (match.id == static_cast<uint64_t>(i - 5 + 100)) ++correct;
        }
        if (correct != 5) throw std::runtime_error("Semantic retrieval smoke set failed");
        std::cout << "Negative-query nearest similarity=" << index.search(embeddings[11], 1).front().cosine_similarity << " (abstention threshold not calibrated)\n";
        std::cout << "PASS cases=" << cases.size() << " dimensions=384 max_absolute_error=" << max_error << " semantic_top1=" << correct << "/5\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
