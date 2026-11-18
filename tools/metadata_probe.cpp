#include "storage/metadata_store.h"
#include "indexing/file_scanner.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <algorithm>
#include <stdexcept>

using namespace filesearcher;
int main(int argc, char* argv[]) {
    QCoreApplication app(argc,argv); QTextStream output(stdout);
    try {
        const auto args = app.arguments();
        if (args.size() == 3 && args[1] == "--demo") {
            const auto base = normalizedPath(args[2]);
            if (QFileInfo::exists(base)) throw std::runtime_error("Demo destination must be a new directory");
            if (!QDir().mkpath(base + "/index") || !QDir().mkpath(base + "/Documents")) throw std::runtime_error("Cannot create demo directory");
            const QList<QPair<QString,QByteArray>> samples{
                {"Algorithms - lecture 19.md", "# Master Theorem\nSynthetic lecture notes for FileSearcher UI verification.\n"},
                {"C++ project notes.txt", "Synthetic project notes: VGA display controller.\n"},
                {"main.cpp", "// Synthetic file search demo\nint main() { return 0; }\n"},
                {"Resume - software engineer.md", "# Resume\nSynthetic fixture; no personal information.\n"},
                {"Monthly expenses.csv", "category,amount\nbooks,42\n"},
                {"Diode study notes.txt", "Synthetic electronics notes about diode characteristics.\n"}
            };
            for (const auto& [name,contents] : samples) {
                QFile file(base + "/Documents/" + name);
                if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) throw std::runtime_error("Cannot write demo fixture");
            }
            MetadataStore store(base + "/index/metadata.sqlite",true);
            store.addRoot(base + "/Documents",{".git","build"});
            std::atomic_bool cancelled{false};
            const auto report = scanRoot(store,store.roots().front(),{base + "/index"},cancelled);
            if (report.errors || report.files != samples.size()) throw std::runtime_error("Demo scan failed");
            output << "Indexed " << report.files << " synthetic files. Data directory: " << base << "/index\n";
        } else if (args.size() == 2 && args[1] == "--benchmark") {
            QTemporaryDir fixture;
            if (!fixture.isValid()) throw std::runtime_error("Cannot create benchmark directory");
            MetadataStore store(fixture.path() + "/metadata.sqlite",true);
            const auto root = store.addRoot(fixture.path() + "/synthetic",{});
            QElapsedTimer timer; timer.start(); store.beginScan();
            const auto now = QDateTime::currentMSecsSinceEpoch();
            for (int i = 0; i < 50000; ++i) {
                const auto extension = i % 2 ? "cpp" : "txt";
                const auto name = QString("report-%1.%2").arg(i,5,10,QChar('0')).arg(extension);
                store.record(root,"benchmark",{fixture.path() + "/synthetic/" + name,name,extension,i,now});
            }
            store.finishScan(root,"benchmark",true);
            output << "Synthetic records: 50000; insertion_ms=" << timer.elapsed() << '\n';
            for (const auto& text : {QString("report-00001.cpp"),QString("report-00"),QString("001"),QString("synthetic/report"),QString("absent"),QString("")}) {
                QList<double> elapsed;
                qint64 total = 0;
                for (int trial = 0; trial < 25; ++trial) {
                    SearchOptions options; options.query = text;
                    timer.restart(); auto result = store.search(options);
                    elapsed << timer.nsecsElapsed() / 1000000.0; total = result.total;
                }
                std::sort(elapsed.begin(),elapsed.end());
                output << "query='" << text << "' count=" << total << " median_ms=" << elapsed[12] << " p95_ms=" << elapsed[23] << '\n';
            }
        } else {
            output << "Usage: metadata_probe --demo <new-directory> | --benchmark\n"; return 2;
        }
    } catch (const std::exception& error) { output << error.what() << '\n'; return 1; }
    return 0;
}
