#include "ai/bert_tokenizer.h"
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>
#include <stdexcept>
namespace filesearcher {
namespace {
bool chinese(char32_t c) {
    return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) ||
        (c >= 0x20000 && c <= 0x2A6DF) || (c >= 0x2A700 && c <= 0x2B73F) ||
        (c >= 0x2B740 && c <= 0x2B81F) || (c >= 0x2B820 && c <= 0x2CEAF) ||
        (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x2F800 && c <= 0x2FA1F);
}
bool punctuation(char32_t c) {
    auto category = QChar::category(c);
    return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) ||
        (c >= 123 && c <= 126) || (category >= QChar::Punctuation_Connector && category <= QChar::Punctuation_Other);
}
QString scalar(char32_t c) { return QString::fromUcs4(&c, 1); }
}
BertTokenizer::BertTokenizer(const QString& vocabulary) {
    QFile file(vocabulary);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Vocabulary unavailable");
    QTextStream stream(&file);
    int64_t id = 0;
    while (!stream.atEnd()) vocabulary_.insert(stream.readLine(), id++);
    if (vocabulary_.value("[CLS]", -1) != 101 || vocabulary_.value("[SEP]", -1) != 102 || vocabulary_.value("[UNK]", -1) != 100)
        throw std::runtime_error("Unexpected BGE vocabulary");
}
std::vector<int64_t> BertTokenizer::encode(const QString& text, bool query, bool truncate) const {
    const QString input = (query ? QStringLiteral("Represent this sentence for searching relevant passages: ") : QString()) + text;
    std::vector<int64_t> result{101};
    auto wordpiece = [&](const QString& token) {
        if (token.isEmpty()) return;
        std::vector<int64_t> pieces;
        if (token.toUcs4().size() > 100) { result.push_back(100); return; }
        for (qsizetype start = 0; start < token.size();) {
            qsizetype end = token.size();
            int64_t id = -1;
            while (end > start) {
                id = vocabulary_.value((start ? "##" : "") + token.mid(start, end - start), -1);
                if (id != -1) break;
                --end;
            }
            if (id == -1) { pieces = {100}; break; }
            pieces.push_back(id);
            start = end;
        }
        result.insert(result.end(), pieces.begin(), pieces.end());
    };
    auto basic = [&](const QString& segment) {
        QString clean;
        for (char32_t c : segment.toUcs4()) {
            const auto cat = QChar::category(c);
            if (c == 0 || c == 0xFFFD) continue;
            if (c == '\t' || c == '\n' || c == '\r' || QChar::isSpace(c)) clean += ' ';
            else if (cat == QChar::Other_Control || cat == QChar::Other_Format) continue;
            else if (chinese(c)) clean += ' ' + scalar(c) + ' ';
            else clean += scalar(c);
        }
        QString normalized;
        for (char32_t c : clean.toLower().normalized(QString::NormalizationForm_D).toUcs4())
            if (QChar::category(c) != QChar::Mark_NonSpacing) normalized += scalar(c);
        QString token;
        for (char32_t c : normalized.toUcs4()) {
            if (QChar::isSpace(c) || punctuation(c)) {
                wordpiece(token); token.clear();
                if (punctuation(c)) wordpiece(scalar(c));
            } else token += scalar(c);
        }
        wordpiece(token);
    };
    static const QRegularExpression special("\\[(?:UNK|SEP|PAD|CLS|MASK)\\]");
    auto matches = special.globalMatch(input);
    qsizetype previous = 0;
    while (matches.hasNext()) {
        const auto match = matches.next();
        basic(input.mid(previous, match.capturedStart() - previous));
        result.push_back(vocabulary_.value(match.captured()));
        previous = match.capturedEnd();
    }
    basic(input.mid(previous));
    if (truncate && result.size() > 511) result.resize(511);
    result.push_back(102);
    return result;
}
}
