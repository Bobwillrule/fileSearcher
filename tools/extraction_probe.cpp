#include "extraction/probe_runner.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 3) {
        std::cerr << "Usage: extraction_probe <worker.exe> <document>\n";
        return 2;
    }
    const auto result = filesearcher::runExtractionProbe(app.arguments()[1], app.arguments()[2]);
    // Report only coverage/counts; do not print extracted personal text.
    const auto json = QJsonDocument(QJsonObject{{"status", result.status}, {"characters", result.text.size()}, {"detail", result.detail}}).toJson();
    std::cout.write(json.constData(), json.size());
    return result.status == "ok" ? 0 : 1;
}
