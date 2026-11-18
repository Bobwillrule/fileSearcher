#include "extraction/resource_limits.h"
#include "extraction/content_types.h"
#include <QGuiApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QBuffer>
#include <QDir>
#include <QStringDecoder>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QXmlStreamReader>
#include <zip.h>
#include <iostream>
#include <memory>
#include <algorithm>
namespace {
int reply(QString status, QString text = {}, QJsonArray sections = {}) {
    const auto json = QJsonDocument(QJsonObject{{"status", status}, {"text", text}, {"sections",sections}}).toJson(QJsonDocument::Compact);
    std::cout.write(json.constData(), json.size());
    // Publish the protocol response before Qt/PDFium teardown begins.
    std::cout.flush();
    return 0;
}
constexpr qsizetype maximum = 1'000'000;
struct Snapshot { QByteArray bytes; QString error; };
Snapshot snapshot(const QString& input, qint64 limit) {
    auto path = QDir::toNativeSeparators(QFileInfo(input).absoluteFilePath());
    if (path.startsWith("\\\\") && !path.startsWith("\\\\?\\")) return {{},"unsupported_location"};
    if (!path.startsWith("\\\\?\\")) path = "\\\\?\\" + path;
    constexpr DWORD excluded = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_DIRECTORY;
    const auto attrs = GetFileAttributesW(path.toStdWString().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return {{},"missing_file"};
    if (attrs & excluded) return {{},"placeholder_or_link"};
    // Sharing only reads prevents replacement or concurrent writes during the snapshot.
    auto handle = CreateFileW(path.toStdWString().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL | FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {{},"read_failed"};
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{handle};
    // A parent directory may have become a junction since metadata enumeration.
    const DWORD needed = GetFinalPathNameByHandleW(handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if (!needed) return {{},"read_failed"};
    std::wstring resolved(needed,L'\0');
    const auto written = GetFinalPathNameByHandleW(handle,resolved.data(),needed,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if (!written || written >= needed) return {{},"read_failed"};
    if (QString::compare(QString::fromWCharArray(resolved.data(),written),path,Qt::CaseInsensitive)) return {{},"placeholder_or_link"};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle,&info)) return {{},"read_failed"};
    if (info.dwFileAttributes & excluded) return {{},"placeholder_or_link"};
    const auto size = (static_cast<quint64>(info.nFileSizeHigh)<<32) | info.nFileSizeLow;
    if (size > static_cast<quint64>(limit)) return {{},"file_too_large"};
    QByteArray bytes(static_cast<qsizetype>(size),Qt::Uninitialized);
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        if (!ReadFile(handle,bytes.data()+offset,static_cast<DWORD>(std::min(qsizetype(65536),bytes.size()-offset)),&read,nullptr) || !read) return {{},"read_failed"};
        offset += read;
    }
    return {bytes,{}};
}
}
int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    filesearcher::ParserLimits limits;
    if (!limits.ready()) return reply("resource_limit_setup_failed");
    if (app.arguments().size() != 2) return reply("invalid_request");
    const QFileInfo info(app.arguments()[1]);
    if (!info.isFile()) return reply("missing_file");
    const bool plain = filesearcher::isTextExtension(info.suffix());
    if (!plain && info.suffix().compare("pdf",Qt::CaseInsensitive) && info.suffix().compare("docx",Qt::CaseInsensitive)) return reply("unsupported_format");
    auto input = snapshot(info.absoluteFilePath(),plain ? 4*1024*1024 : 32*1024*1024);
    if (!input.error.isEmpty()) return reply(input.error);
    if (plain) {
        auto encoding = QStringConverter::Utf8;
        const auto& data = input.bytes;
        if (data.startsWith(QByteArray::fromHex("fffe0000"))) encoding = QStringConverter::Utf32LE;
        else if (data.startsWith(QByteArray::fromHex("0000feff"))) encoding = QStringConverter::Utf32BE;
        else if (data.startsWith(QByteArray::fromHex("fffe"))) encoding = QStringConverter::Utf16LE;
        else if (data.startsWith(QByteArray::fromHex("feff"))) encoding = QStringConverter::Utf16BE;
        QStringDecoder decoder(encoding,QStringConverter::Flag::Stateless);
        QString text = decoder(data);
        if (decoder.hasError()) return reply("unsupported_encoding");
        for (const auto c : text) if (c.unicode() < 32 && c != '\n' && c != '\r' && c != '\t' && c != '\f') return reply("binary_data");
        text.replace("\r\n","\n").replace('\r','\n');
        const bool truncated = text.size() > maximum;
        if (truncated) { text.truncate(maximum); if (text.back().isHighSurrogate()) text.chop(1); }
        return reply(truncated ? "text_limit" : text.trimmed().isEmpty() ? "no_text" : "ok",text);
    }
    if (info.suffix().compare("pdf", Qt::CaseInsensitive) == 0) {
        QBuffer buffer(&input.bytes); buffer.open(QIODevice::ReadOnly);
        QPdfDocument document;
        document.load(&buffer);
        auto error = document.error();
        if (error == QPdfDocument::Error::IncorrectPassword) return reply("password_protected");
        if (error != QPdfDocument::Error::None) return reply("invalid_document");
        if (document.pageCount() > 10000) return reply("page_limit");
        QString text;
        QJsonArray sections;
        for (int page = 0; page < document.pageCount(); ++page) {
            const auto passage = document.getAllText(page).text();
            auto kept = passage.left(maximum-text.size());
            if (!kept.isEmpty() && kept.back().isHighSurrogate()) kept.chop(1);
            sections << QJsonObject{{"offset",text.size()},{"length",kept.size()},{"page",page+1}};
            text += kept;
            if (kept.size() < passage.size() || text.size() >= maximum) return reply("text_limit",text,sections);
            text += '\n';
        }
        return reply(text.trimmed().isEmpty() ? "no_text_or_ocr_required" : "ok", text,sections);
    }
    if (info.suffix().compare("docx", Qt::CaseInsensitive) == 0) {
        // Read in memory: no archive entries are ever extracted to disk.
        auto& data = input.bytes;
        zip_error_t error;
        zip_error_init(&error);
        auto source = zip_source_buffer_create(data.constData(), static_cast<zip_uint64_t>(data.size()), 0, &error);
        zip_error_fini(&error);
        if (!source) return reply("invalid_document");
        zip_t* raw = zip_open_from_source(source, ZIP_RDONLY, nullptr);
        if (!raw) { zip_source_free(source); return reply("invalid_document"); }
        std::unique_ptr<zip_t, decltype(&zip_discard)> archive(raw, zip_discard);
        zip_stat_t stat{};
        if (zip_stat(raw, "word/document.xml", 0, &stat) != 0) return reply("invalid_document");
        if (stat.size > 8 * 1024 * 1024) return reply("xml_limit");
        if (stat.encryption_method != ZIP_EM_NONE) return reply("password_protected");
        std::unique_ptr<zip_file_t, decltype(&zip_fclose)> entry(zip_fopen(raw, "word/document.xml", 0), zip_fclose);
        if (!entry) return reply("invalid_document");
        QByteArray xml(static_cast<qsizetype>(stat.size), Qt::Uninitialized);
        zip_uint64_t offset = 0;
        while (offset < stat.size) {
            auto read = zip_fread(entry.get(), xml.data() + offset, stat.size - offset);
            if (read <= 0) return reply("invalid_document");
            offset += static_cast<zip_uint64_t>(read);
        }
        QXmlStreamReader reader(xml);
        reader.setEntityExpansionLimit(0);
        QString text;
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isDTD() || reader.isEntityReference()) return reply("unsupported_xml");
            const bool word = reader.namespaceUri() == "http://schemas.openxmlformats.org/wordprocessingml/2006/main" || reader.namespaceUri() == "http://purl.oclc.org/ooxml/wordprocessingml/main";
            if (word && reader.isStartElement() && reader.name() == "t") text += reader.readElementText();
            else if (word && reader.isEndElement() && reader.name() == "p") text += '\n';
            else if (word && reader.isStartElement() && reader.name() == "tab") text += '\t';
            else if (word && reader.isStartElement() && (reader.name() == "br" || reader.name() == "cr")) text += '\n';
            if (text.size() > maximum) {
                text.truncate(maximum); if (text.back().isHighSurrogate()) text.chop(1);
                return reply("text_limit",text,QJsonArray{QJsonObject{{"offset",0},{"length",text.size()},{"label","Document body"}}});
            }
        }
        if (reader.hasError()) return reply("invalid_document");
        return reply(text.trimmed().isEmpty() ? "no_text" : "ok", text,QJsonArray{QJsonObject{{"offset",0},{"length",text.size()},{"label","Document body"}}});
    }
    return reply("unsupported_format");
}
