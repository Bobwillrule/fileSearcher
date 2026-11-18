#include "search/vector_index.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <array>
#include <chrono>
#include <limits>
#include <fstream>
#include <cmath>

using filesearcher::VectorIndex;
namespace {
constexpr std::array<float, 3> x{1, 0, 0};
constexpr std::array<float, 3> y{0, 1, 0};
struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        const auto base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            path = base / ("filesearcher-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(path)) return;
        }
        throw std::runtime_error("Cannot allocate isolated test directory");
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
}
TEST_CASE("Normalized cosine lookup preserves stable 64-bit chunk IDs") {
    VectorIndex index(3, 10);
    index.add(9000000001ULL, std::array<float, 3>{10, 0, 0});
    index.add(7, y);
    auto results = index.search(x, 10);
    REQUIRE(results.size() == 2);
    CHECK(results.front().id == 9000000001ULL);
    CHECK(results.front().cosine_similarity == Catch::Approx(1));
    CHECK(index.search(x, 0).empty());
}
TEST_CASE("Updating a label replaces its vector without duplicate results") {
    VectorIndex index(3, 10);
    index.add(42, x);
    index.add(42, y);
    auto results = index.search(y, 10);
    REQUIRE(results.size() == 1);
    CHECK(results[0].cosine_similarity == Catch::Approx(1));
}
TEST_CASE("Deletion survives save and reload and a missing cache preserves live state") {
    TemporaryDirectory temporary;
    const auto cache = temporary.path / "index.hnsw";
    VectorIndex original(3, 10);
    original.add(42, x);
    original.add(81, y);
    original.remove(42);
    original.remove(42);
    original.save(cache);
    VectorIndex restored(3, 10);
    restored.load(cache);
    auto results = restored.search(x, 10);
    REQUIRE(results.size() == 1);
    CHECK(results[0].id == 81);
    REQUIRE_THROWS(restored.load(temporary.path / "missing.hnsw"));
    CHECK(restored.search(y, 1).front().id == 81);
    restored.remove(81);
    CHECK(restored.search(x, 10).empty());
}
TEST_CASE("Invalid vectors fail before reaching the index") {
    VectorIndex index(3, 10);
    CHECK(index.search(x, 2).empty());
    REQUIRE_THROWS_AS(index.add(1, std::array<float, 2>{1, 0}), std::invalid_argument);
    REQUIRE_THROWS_AS(index.add(1, std::array<float, 3>{0, 0, 0}), std::invalid_argument);
    REQUIRE_THROWS_AS(index.add(1, std::array<float, 3>{std::numeric_limits<float>::quiet_NaN(), 0, 0}), std::invalid_argument);
    REQUIRE_THROWS_AS(index.search(std::array<float, 3>{0, 0, 0}, 1), std::invalid_argument);
}
TEST_CASE("Graph cache roundtrip preserves rankings and rejects malformed records") {
    TemporaryDirectory temporary; const auto cache=temporary.path / L"cache-\u6771\u4eac.hnsw";
    VectorIndex original(3,200);
    for (int i=0;i<150;++i) original.add(i+100,std::array<float,3>{std::sin(i*0.3F),std::cos(i*0.3F),i*0.01F});
    for (int i=0;i<150;i+=7) original.remove(i+100);
    original.save(cache); VectorIndex restored(3,200); restored.load(cache);
    for (int i=0;i<20;++i) {
        const std::array<float,3> query{std::sin(i*0.5F),0.2F,std::cos(i*0.5F)};
        auto before=original.search(query,10),after=restored.search(query,10); REQUIRE(before.size()==after.size());
        for (size_t j=0;j<before.size();++j) { CHECK(before[j].id==after[j].id); CHECK(before[j].cosine_similarity==after[j].cosine_similarity); }
    }
    { std::fstream file(cache,std::ios::binary|std::ios::in|std::ios::out); file.seekp(12); const uint32_t oversized=0xffffffff; file.write(reinterpret_cast<const char*>(&oversized),4); }
    CHECK_THROWS(restored.load(cache)); CHECK(restored.size()==original.size());
    original.save(cache); { std::ofstream file(cache,std::ios::binary|std::ios::app); file.put('x'); }
    CHECK_THROWS(restored.load(cache));
    original.save(cache); std::filesystem::resize_file(cache,25); CHECK_THROWS(restored.load(cache));
    original.save(cache); VectorIndex wrongDimension(4,200); CHECK_THROWS(wrongDimension.load(cache));
}
