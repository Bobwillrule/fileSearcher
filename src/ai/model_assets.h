#pragma once
#include "ai/bert_tokenizer.h"
#include <QList>
namespace filesearcher {
inline constexpr int embeddingDimensions = 384;
QString modelFingerprint();
void verifyModelAssets(const QString& directory);
struct EmbeddingPart { int offset, length, tokens; QString text; };
QList<EmbeddingPart> embeddingParts(const QString& text, const BertTokenizer& tokenizer);
}
