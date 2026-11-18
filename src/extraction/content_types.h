#pragma once
#include <QString>
#include <QList>

namespace filesearcher {
struct TextSection { QString text; int page = 0; QString label; };
struct Passage { QString text; QString location; int tokens = 0; };
bool isTextExtension(const QString& extension);
QList<Passage> splitPassages(const QList<TextSection>& sections);
}
