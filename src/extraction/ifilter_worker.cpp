#define NOMINMAX
#include <windows.h>
#include <filter.h>
#include <filterr.h>
#include <ntquery.h>
#include <wrl/client.h>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include "extraction/resource_limits.h"

namespace {
int reply(const QString& status, const QString& text = {}, const QString& detail = {}) {
    const auto data = QJsonDocument(QJsonObject{{"status", status}, {"text", text}, {"detail", detail}}).toJson(QJsonDocument::Compact);
    std::cout.write(data.constData(), data.size());
    std::cout.flush();
    return 0;
}
QString describe(HRESULT hr) { return "HRESULT 0x" + QString::number(static_cast<quint32>(hr), 16); }
int failure(HRESULT hr, const QString& fallback) {
    return reply(hr == FILTER_E_PASSWORD ? "password_protected" : fallback, {}, describe(hr));
}
struct ComScope { ~ComScope() { CoUninitialize(); } };
}
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    filesearcher::ParserLimits limits;
    if (!limits.ready()) return reply("resource_limit_setup_failed");
    // Text retained by this worker is bounded. Parser-internal allocation is not.
    // The parent enforces wall-clock timeout.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (app.arguments().size() != 2) return reply("invalid_request");
    const QFileInfo input(app.arguments()[1]);
    if (!input.isFile()) return reply("missing_file");
    if (input.size() > 32 * 1024 * 1024) return reply("file_too_large");
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return failure(initialized, "initialization_failed");
    ComScope com;
    Microsoft::WRL::ComPtr<IFilter> filter;
    const auto path = input.absoluteFilePath().toStdWString();
    HRESULT hr = LoadIFilter(path.c_str(), nullptr, reinterpret_cast<void**>(filter.GetAddressOf()));
    if (FAILED(hr)) return failure(hr, "filter_unavailable_or_load_failed");
    ULONG flags = 0;
    hr = filter->Init(IFILTER_INIT_CANON_PARAGRAPHS | IFILTER_INIT_CANON_SPACES | IFILTER_INIT_APPLY_INDEX_ATTRIBUTES, 0, nullptr, &flags);
    if (FAILED(hr)) return failure(hr, "extraction_failed");
    QString text;
    constexpr qsizetype max_characters = 1'000'000;
    STAT_CHUNK chunk{};
    for (std::size_t chunks = 0; chunks < 100'000; ++chunks) {
        hr = filter->GetChunk(&chunk);
        if (hr == FILTER_E_END_OF_CHUNKS)
            return reply(text.trimmed().isEmpty() ? "no_text_or_ocr_required" : "ok", text);
        if (FAILED(hr)) return failure(hr, "extraction_failed");
        if (!(chunk.flags & CHUNK_TEXT)) continue;
        if (!text.isEmpty()) text += '\n';
        while (true) {
            wchar_t buffer[4096];
            ULONG count = 4096;
            hr = filter->GetText(&count, buffer);
            if (hr == FILTER_E_NO_MORE_TEXT) break;
            if (FAILED(hr)) return failure(hr, "extraction_failed");
            if (count > 4096) return reply("invalid_filter_output");
            if (text.size() + count > max_characters) return reply("text_limit", text);
            text += QString::fromWCharArray(buffer, static_cast<qsizetype>(count));
            if (hr == FILTER_S_LAST_TEXT) break;
            if (count == 0) return reply("invalid_filter_output");
        }
    }
    return reply("chunk_limit", text);
}
