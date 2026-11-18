#include "ai/model_assets.h"
#include <QCryptographicHash>
#include <QFile>
#include <stdexcept>
namespace filesearcher {
QString modelFingerprint() {
    // Changing any semantic transformation requires a new fingerprint.
    static const auto fingerprint = QCryptographicHash::hash(QByteArrayLiteral(
        "BAAI/bge-small-en-v1.5@5c38ec7c405ec4b44b94cc5a9bb96e735b38267a|"
        "onnx:828e1496d7fabb79cfa4dcd84fa38625c0d3d21da474a00f08db0f559940cf35|"
        "vocab:07eced375cec144d27c900241f3e339478dec958f92fddbc551f295c992038a3|"
        "bert-uncased-v1|CLS|L2|384|split384-v1|ORT1.23.2|"
        "Represent this sentence for searching relevant passages: "),QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(fingerprint);
}
void verifyModelAssets(const QString& directory) {
    const QList<QPair<QString,QByteArray>> assets{
        {"onnx/model.onnx","828e1496d7fabb79cfa4dcd84fa38625c0d3d21da474a00f08db0f559940cf35"},
        {"vocab.txt","07eced375cec144d27c900241f3e339478dec958f92fddbc551f295c992038a3"},
        {"README.md","ddb964361a55c6e5dfca6361615854b260c9c960205d04c7520151aaa1d75837"}};
    for (const auto& [name,expected] : assets) {
        QFile file(directory+'/'+name); QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file) || hash.result().toHex()!=expected)
            throw std::runtime_error("Local AI model is missing or failed checksum verification. Prepare the pinned model and rebuild.");
    }
}
QList<EmbeddingPart> embeddingParts(const QString& text,const BertTokenizer& tokenizer) {
    QList<EmbeddingPart> result;
    // Recursive contiguous partition preserves all source text and UTF-16 offsets.
    const auto split = [&](auto&& self,int offset,int length) -> void {
        const auto part=text.mid(offset,length);
        const auto tokens=tokenizer.encode(part,false,false).size();
        if (tokens<=386) { result << EmbeddingPart{offset,length,static_cast<int>(tokens),part}; return; }
        int middle=length/2;
        for (int i=middle;i>length/3;--i) if (part[i].isSpace()) { middle=i+1; break; }
        if (part[middle].isLowSurrogate()) --middle;
        if (middle<=0 || middle>=length) throw std::runtime_error("Cannot split embedding passage");
        self(self,offset,middle); self(self,offset+middle,length-middle);
    };
    if (!text.isEmpty()) split(split,0,static_cast<int>(text.size()));
    return result;
}
}
