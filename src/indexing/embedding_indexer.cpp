#include "indexing/embedding_indexer.h"
#include "ai/embedding_client.h"
#include "storage/embedding_store.h"
#include "search/vector_cache.h"
#include <algorithm>
namespace filesearcher {
EmbeddingReport indexEmbeddings(const QString& database,qint64 root,const QString& worker,const QString& model,
        const QString& cache,int profile,const std::atomic_bool& cancelled,const std::atomic_bool& disabled,const std::function<void(int)>& progress) {
    EmbeddingReport report;
    try {
        const auto stop=[&] { if (cancelled || disabled) throw std::runtime_error("AI indexing stopped; completed vectors are saved."); };
        stop(); verifyModelAssets(model);
        BertTokenizer tokenizer(model+"/vocab.txt"); EmbeddingStore store(database);
        profile=std::clamp(profile,0,2);
        const int threads[]={1,2,4},batches[]={1,4,8};
        std::unique_ptr<EmbeddingClient> client;
        // Prepare a bounded page, drain it, then fetch another page. No corpus-sized text buffer.
        for (;;) {
            stop(); const auto passages=store.pendingPassages(root);
            for (const auto& passage:passages) { stop(); store.prepare(passage,embeddingParts(passage.text,tokenizer)); }
            for (;;) {
                stop(); const auto batch=store.pending(root,batches[profile]); if (batch.isEmpty()) break;
                if (!client) client=std::make_unique<EmbeddingClient>(worker,model,threads[profile],cancelled,disabled);
                QStringList texts; for (const auto& item:batch) texts << item.text;
                const auto vectors=client->embed(texts); stop(); report.embedded+=store.commit(batch,vectors);
                if (progress) progress(report.embedded);
            }
            if (passages.isEmpty()) break;
        }
        client.reset(); stop();
        auto recovered=recoverVectorCache(store,cache,cancelled,disabled);
        report.rebuilt=recovered.rebuilt; report.total=store.count();
        report.status=QString("Local AI ready: %1 passages saved (%2 new).").arg(report.total).arg(report.embedded);
    } catch (const std::exception& error) {
        report.cancelled=cancelled || disabled; report.status=QString::fromUtf8(error.what());
    }
    return report;
}
}
