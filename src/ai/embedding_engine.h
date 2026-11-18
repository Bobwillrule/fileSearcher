#pragma once
#include <QString>
#include <memory>
#include <vector>
#include <cstdint>
namespace filesearcher {
class EmbeddingEngine {
public:
    EmbeddingEngine(const QString& directory,int threads);
    ~EmbeddingEngine();
    std::vector<std::vector<float>> embed(const std::vector<std::vector<int64_t>>& tokens);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
