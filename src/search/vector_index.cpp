#include "search/vector_index.h"
#include <hnswlib/hnswlib.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <unordered_set>
#include <bit>
#include <cstring>

namespace filesearcher {
namespace {
template<class T> void write(std::ostream& out,T value) { out.write(reinterpret_cast<const char*>(&value),sizeof(value)); }
template<class T> T read(std::istream& in) {
    T value{}; if (!in.read(reinterpret_cast<char*>(&value),sizeof(value))) throw std::runtime_error("Truncated vector cache"); return value;
}
std::vector<float> normalized(std::span<const float> input, std::size_t dimensions) {
    if (input.size() != dimensions) throw std::invalid_argument("Vector dimension mismatch");
    double squared_norm = 0;
    for (const float value : input) {
        if (!std::isfinite(value)) throw std::invalid_argument("Vector must be finite");
        squared_norm += static_cast<double>(value) * value;
    }
    if (squared_norm == 0) throw std::invalid_argument("Zero vector has no cosine direction");
    const double norm = std::sqrt(squared_norm);
    std::vector<float> output;
    output.reserve(dimensions);
    for (const float value : input) output.push_back(static_cast<float>(value / norm));
    return output;
}
}
struct VectorIndex::Impl {
    std::size_t dimensions;
    std::size_t capacity;
    hnswlib::InnerProductSpace space;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> graph;
    std::unordered_set<ChunkId> live;
    Impl(std::size_t dims, std::size_t cap) : dimensions(dims), capacity(cap), space(dims),
        graph(std::make_unique<hnswlib::HierarchicalNSW<float>>(&space, cap, 16, 100, 42)) {
        graph->setEf(64);
    }
};
VectorIndex::VectorIndex(std::size_t dimensions, std::size_t capacity) {
    if (dimensions == 0 || capacity == 0) throw std::invalid_argument("Dimensions and capacity must be positive");
    static_assert(sizeof(hnswlib::labeltype) >= sizeof(ChunkId), "64-bit target required");
    impl_ = std::make_unique<Impl>(dimensions, capacity);
}
VectorIndex::~VectorIndex() = default;
bool VectorIndex::contains(ChunkId id) const { return impl_->live.contains(id); }
std::size_t VectorIndex::size() const { return impl_->live.size(); }
void VectorIndex::add(ChunkId id, std::span<const float> vector) {
    auto values = normalized(vector, impl_->dimensions);
    impl_->graph->addPoint(values.data(), static_cast<hnswlib::labeltype>(id));
    impl_->live.insert(id);
}
void VectorIndex::remove(ChunkId id) {
    if (!impl_->live.contains(id)) return;
    impl_->graph->markDelete(static_cast<hnswlib::labeltype>(id));
    impl_->live.erase(id);
}
std::vector<VectorMatch> VectorIndex::search(std::span<const float> query, std::size_t count) const {
    auto values = normalized(query, impl_->dimensions);
    count = std::min(count, impl_->live.size());
    if (count == 0) return {};
    auto candidates = impl_->graph->searchKnn(values.data(), count);
    std::vector<VectorMatch> result;
    while (!candidates.empty()) {
        auto [distance, label] = candidates.top();
        candidates.pop();
        result.push_back({static_cast<ChunkId>(label), std::clamp(1.0F - distance, -1.0F, 1.0F)});
    }
    std::reverse(result.begin(), result.end());
    return result;
}
void VectorIndex::save(const std::filesystem::path& path) const {
    static_assert(std::endian::native==std::endian::little);
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    const auto& graph=*impl_->graph;
    out.write("FSHNSW01",8); write<uint32_t>(out,static_cast<uint32_t>(impl_->dimensions));
    write<uint32_t>(out,static_cast<uint32_t>(graph.cur_element_count.load()));
    write<uint32_t>(out,graph.enterpoint_node_); write<int32_t>(out,graph.maxlevel_);
    for (uint32_t i=0;i<graph.cur_element_count;++i) {
        write<uint64_t>(out,graph.getExternalLabel(i)); write<uint32_t>(out,graph.element_levels_[i]);
        write<uint32_t>(out,graph.isMarkedDeleted(i)?1:0);
        out.write(graph.getDataByInternalId(i),impl_->dimensions*sizeof(float));
        for (int level=0;level<=graph.element_levels_[i];++level) {
            auto* links=graph.get_linklist_at_level(i,level); const auto count=graph.getListCount(links);
            write<uint32_t>(out,count);
            for (unsigned j=0;j<count;++j) write<uint32_t>(out,links[j+1]);
        }
    }
    out.flush(); if (!out) throw std::runtime_error("Vector cache could not be written");
}
void VectorIndex::load(const std::filesystem::path& path) {
    // Avoid hnswlib's unchecked native loader and narrow filename API. This wrapper
    // stores only bounded node records, never serialized allocation sizes/pointers.
    if (std::filesystem::file_size(path)>512ULL*1024*1024) throw std::runtime_error("Vector cache exceeds limit");
    std::ifstream in(path,std::ios::binary); char magic[8]{}; in.read(magic,8);
    if (std::memcmp(magic,"FSHNSW01",8)!=0 || read<uint32_t>(in)!=impl_->dimensions) throw std::runtime_error("Vector cache format mismatch");
    const auto count=read<uint32_t>(in),entry=read<uint32_t>(in); const auto maxlevel=read<int32_t>(in);
    if (count>100000 || (count && (entry>=count || maxlevel<0 || maxlevel>32)) || (!count && maxlevel!=-1) ||
        static_cast<uint64_t>(count)*impl_->dimensions*4>256ULL*1024*1024) throw std::runtime_error("Invalid graph header");
    auto replacement=std::make_unique<Impl>(impl_->dimensions,std::max<size_t>(impl_->capacity,count));
    auto& graph=*replacement->graph;
    graph.enterpoint_node_=entry; graph.maxlevel_=maxlevel;
    for (uint32_t i=0;i<count;++i) {
        const auto label=read<uint64_t>(in); const auto level=read<uint32_t>(in),deleted=read<uint32_t>(in);
        if (level>32 || static_cast<int>(level)>maxlevel || deleted>1 || graph.label_lookup_.contains(label)) throw std::runtime_error("Invalid graph node");
        std::memset(graph.data_level0_memory_+i*graph.size_data_per_element_,0,graph.size_data_per_element_);
        graph.linkLists_[i]=nullptr;
        if (level) {
            graph.linkLists_[i]=static_cast<char*>(std::calloc(level,graph.size_links_per_element_));
            if (!graph.linkLists_[i]) throw std::bad_alloc();
        }
        graph.element_levels_[i]=level; graph.cur_element_count=i+1;
        graph.setExternalLabel(i,label); graph.label_lookup_[label]=i;
        double squared=0;
        for (size_t j=0;j<impl_->dimensions;++j) {
            const auto value=read<float>(in); squared+=static_cast<double>(value)*value;
            std::memcpy(graph.getDataByInternalId(i)+j*sizeof(float),&value,sizeof(float));
        }
        if (!std::isfinite(squared) || std::abs(squared-1)>0.001) throw std::runtime_error("Invalid cached vector");
        for (unsigned l=0;l<=level;++l) {
            const auto degree=read<uint32_t>(in);
            if (degree>(l?16U:32U)) throw std::runtime_error("Invalid graph degree");
            auto* links=graph.get_linklist_at_level(i,l); graph.setListCount(links,static_cast<unsigned short>(degree));
            for (unsigned j=0;j<degree;++j) {
                const auto neighbor=read<uint32_t>(in);
                if (neighbor>=count || neighbor==i) throw std::runtime_error("Invalid graph edge");
                links[j+1]=neighbor;
            }
        }
        if (deleted) graph.markDeletedInternal(i); else replacement->live.insert(label);
    }
    if (in.peek()!=std::char_traits<char>::eof() || (count && graph.element_levels_[entry]!=maxlevel)) throw std::runtime_error("Invalid graph trailer");
    for (uint32_t i=0;i<count;++i) for (int level=1;level<=graph.element_levels_[i];++level) {
        auto* links=graph.get_linklist_at_level(i,level);
        for (unsigned j=0;j<graph.getListCount(links);++j)
            if (graph.element_levels_[links[j+1]]<level) throw std::runtime_error("Invalid graph edge level");
    }
    impl_=std::move(replacement);
}
}
