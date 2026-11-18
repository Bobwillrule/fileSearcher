#include "extraction/content_types.h"
#include <QSet>
#include <QRegularExpression>
#include <algorithm>

namespace filesearcher {
bool isTextExtension(const QString& extension) {
    static const QSet<QString> extensions{"txt","md","markdown","cpp","cc","cxx","c","h","hpp","hxx","py","js","jsx","ts","tsx","json","jsonl","csv","log","ini","yaml","yml","toml","xml","html","htm","css","sql","rs","go","java","cs","sh","ps1","cmake"};
    return extensions.contains(extension.toLower());
}
QList<Passage> splitPassages(const QList<TextSection>& sections) {
    QList<Passage> result;
    // Model-independent lexical tokens. Long runs are split at Unicode scalar boundaries.
    // Phase 5 must re-budget these passages with its exact model tokenizer.
    static const QRegularExpression lexical("[\\p{L}\\p{N}\\p{M}_]+|[^\\s]",QRegularExpression::UseUnicodePropertiesOption);
    for (const auto& section : sections) {
        struct Token { qsizetype start, end; };
        QList<Token> tokens;
        auto matches = lexical.globalMatch(section.text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            for (auto start = match.capturedStart(); start < match.capturedEnd();) {
                auto end = std::min(start+qsizetype(16),match.capturedEnd());
                if (end < section.text.size() && section.text[end-1].isHighSurrogate()) --end;
                tokens << Token{start,end}; start = end;
            }
        }
        int line = 1; qsizetype counted = 0;
        for (qsizetype first=0; first<tokens.size();) {
            auto last = std::min(first+qsizetype(350),tokens.size());
            while (last > first+1 && tokens[last-1].end-tokens[first].start > 4000) --last;
            const auto start = tokens[first].start, end = tokens[last-1].end;
            line += section.text.mid(counted,start-counted).count('\n'); counted = start;
            const auto text = section.text.mid(start,end-start);
            const auto location = section.page > 0 ? QString("Page %1").arg(section.page) :
                QString("%1lines %2–%3").arg(section.label.isEmpty() ? QString() : section.label + ": ").arg(line).arg(line+text.count('\n'));
            result << Passage{text,location,static_cast<int>(last-first)};
            if (last == tokens.size()) break;
            first = std::max(first+1,last-40);
        }
    }
    return result;
}
}
