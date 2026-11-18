#include "extraction/probe_runner.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>

namespace filesearcher {
ProbeResult runExtractionProbe(const QString& executable, const QString& input, int timeout_ms, const std::atomic_bool* cancelled) {
    if (timeout_ms <= 0) return {"invalid_request", {}, "Timeout must be positive"};
    if (cancelled && cancelled->load()) return {"cancelled",{}, {}};
    QProcess process;
    process.setProgram(executable);
    process.setArguments({input});
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    QElapsedTimer timer;
    timer.start();
    if (!process.waitForStarted(timeout_ms)) return {"start_failed", {}, "Worker did not start"};
    QByteArray output;
    constexpr qsizetype max_output = 8 * 1024 * 1024;
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(20);
        output += process.readAllStandardOutput();
        process.readAllStandardError(); // Do not log arbitrary document/parser output.
        if (output.size() > max_output || timer.elapsed() >= timeout_ms || (cancelled && cancelled->load())) {
            process.kill();
            process.waitForFinished(2000);
            return {cancelled && cancelled->load() ? "cancelled" : output.size() > max_output ? "output_limit" : "timeout", {}, "Worker stopped"};
        }
    }
    output += process.readAllStandardOutput();
    if (output.size() > max_output) return {"output_limit", {}, "Worker output exceeded limit"};
    if (process.exitStatus() == QProcess::CrashExit || process.exitCode() != 0)
        return {"worker_failed", {}, "Worker exited unsuccessfully"};
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(output, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return {"invalid_response", {}, "Worker returned an invalid response"};
    const auto object = document.object();
    const auto status = object.value("status").toString();
    if (status.isEmpty()) return {"invalid_response", {}, "Worker status missing"};
    ProbeResult result{status,object.value("text").toString(),object.value("detail").toString()};
    if (result.text.size() > 1000000) return {"invalid_response",{},"Text limit exceeded"};
    qsizetype previous = 0;
    for (const auto& value : object.value("sections").toArray()) {
        const auto section = value.toObject();
        const auto offset = section.value("offset").toInteger(-1), length = section.value("length").toInteger(-1);
        if (offset < previous || length < 0 || offset > result.text.size() || length > result.text.size()-offset)
            return {"invalid_response",{},"Invalid text sections"};
        result.sections << TextSection{result.text.mid(offset,length),section.value("page").toInt(),section.value("label").toString()};
        previous = offset+length;
    }
    if (result.sections.isEmpty() && !result.text.isEmpty()) result.sections << TextSection{result.text,0,{}};
    return result;
}
}
