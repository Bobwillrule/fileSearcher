#include "extraction/probe_runner.h"
#include <QCoreApplication>
#include <iostream>
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 3) return 2;
    const auto worker = app.arguments()[1], folder = app.arguments()[2];
    struct Case { const char* name; const char* status; const char* content; };
    const Case cases[] = {
        {"text.pdf", "ok", "Master Theorem"}, {"notes.docx", "ok", "Dynamic programming"},
        {"encrypted.pdf", "password_protected", ""}, {"scanned.pdf", "no_text_or_ocr_required", ""},
        {"no_text.pdf", "no_text_or_ocr_required", ""}, {"malformed.pdf", "invalid_document", ""},
        {"malformed.docx", "invalid_document", ""}, {"oversized.docx", "xml_limit", ""},
        {"unknown.filesearcherfixture", "unsupported_format", ""}, {"missing.pdf", "missing_file", ""}
    };
    for (auto c : cases) {
        const auto result = filesearcher::runExtractionProbe(worker, folder + '/' + c.name);
        std::cout << c.name << ": " << result.status.toStdString() << '\n';
        if (result.status != c.status || !result.text.contains(c.content)) return 1;
    }
    // A short error response used to be lost intermittently during worker teardown.
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (filesearcher::runExtractionProbe(worker,folder + "/malformed.pdf").status != "invalid_document") return 1;
    }
    std::cout << "Malformed PDF response delivery: 30/30\n";
    return 0;
}
