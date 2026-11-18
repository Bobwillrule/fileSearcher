#pragma once
#include <QHash>
#include <QString>
#include <vector>
#include <cstdint>
namespace filesearcher {
// Implements the pinned uncased BGE BERT WordPiece configuration, not arbitrary tokenizers.
class BertTokenizer {
public:
    explicit BertTokenizer(const QString& vocabulary);
    std::vector<int64_t> encode(const QString& text, bool query = false, bool truncate = true) const;
private:
    QHash<QString, int64_t> vocabulary_;
};
}
