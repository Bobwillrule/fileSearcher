#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace filesearcher {
using ChunkId = std::uint64_t;
struct VectorMatch {
    ChunkId id;
    float cosine_similarity;
};

// Single-owner HNSW adapter; callers serialize access. Cache format is versioned,
// bounded and validated before publishing a loaded graph. Model identity lives in VectorCache.
class VectorIndex {
public:
    VectorIndex(std::size_t dimensions, std::size_t capacity);
    ~VectorIndex();
    VectorIndex(const VectorIndex&) = delete;
    VectorIndex& operator=(const VectorIndex&) = delete;
    void add(ChunkId id, std::span<const float> vector);
    void remove(ChunkId id);
    [[nodiscard]] std::vector<VectorMatch> search(std::span<const float> query, std::size_t count) const;
    void save(const std::filesystem::path& path) const;
    void load(const std::filesystem::path& path);
    bool contains(ChunkId id) const;
    std::size_t size() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
