#include "ai/embedding_engine.h"
#include "ai/model_assets.h"
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
namespace filesearcher {
struct EmbeddingEngine::Impl {
    Ort::Env environment{ORT_LOGGING_LEVEL_ERROR,"filesearcher"};
    Ort::Session session{nullptr};
    std::string outputName;
    Impl(const QString& directory,int threads) {
        verifyModelAssets(directory);
        environment.DisableTelemetryEvents();
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(std::clamp(threads,1,4)); options.SetInterOpNumThreads(1);
        options.DisableCpuMemArena(); options.DisableMemPattern();
        session=Ort::Session(environment,(directory+"/onnx/model.onnx").toStdWString().c_str(),options);
        Ort::AllocatorWithDefaultOptions allocator;
        outputName=session.GetOutputNameAllocated(0,allocator).get();
    }
};
EmbeddingEngine::EmbeddingEngine(const QString& directory,int threads):impl_(std::make_unique<Impl>(directory,threads)) {}
EmbeddingEngine::~EmbeddingEngine()=default;
std::vector<std::vector<float>> EmbeddingEngine::embed(const std::vector<std::vector<int64_t>>& batch) {
    if (batch.empty() || batch.size()>8) throw std::runtime_error("Invalid embedding batch size");
    size_t length=0;
    for (const auto& tokens:batch) {
        if (tokens.size()<2 || tokens.size()>512) throw std::runtime_error("Embedding token limit exceeded");
        for (auto id:tokens) if (id<0 || id>=30522) throw std::runtime_error("Invalid token ID");
        length=std::max(length,tokens.size());
    }
    std::vector<int64_t> ids(batch.size()*length,0),mask(ids.size(),0),types(ids.size(),0);
    for (size_t i=0;i<batch.size();++i) {
        std::copy(batch[i].begin(),batch[i].end(),ids.begin()+i*length);
        std::fill_n(mask.begin()+i*length,batch[i].size(),1);
    }
    const std::array<int64_t,2> shape{static_cast<int64_t>(batch.size()),static_cast<int64_t>(length)};
    auto memory=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
    std::vector<Ort::Value> inputs;
    for (auto* data:{&ids,&mask,&types}) inputs.push_back(Ort::Value::CreateTensor<int64_t>(memory,data->data(),data->size(),shape.data(),2));
    const char* names[]={"input_ids","attention_mask","token_type_ids"};
    const char* outputs[]={impl_->outputName.c_str()};
    auto output=impl_->session.Run(Ort::RunOptions{nullptr},names,inputs.data(),inputs.size(),outputs,1);
    const auto dims=output[0].GetTensorTypeAndShapeInfo().GetShape();
    if (dims!=std::vector<int64_t>{shape[0],shape[1],384}) throw std::runtime_error("Unexpected embedding output");
    const float* data=output[0].GetTensorData<float>();
    std::vector<std::vector<float>> result;
    for (size_t i=0;i<batch.size();++i) {
        std::vector<float> vector(data+i*length*384,data+i*length*384+384);
        double squared=0;
        for (float value:vector) squared+=static_cast<double>(value)*value;
        if (!(squared>0) || !std::isfinite(squared)) throw std::runtime_error("Invalid embedding output");
        for (auto& value:vector) value=static_cast<float>(value/std::sqrt(squared));
        result.push_back(std::move(vector));
    }
    return result;
}
}
