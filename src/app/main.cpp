#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QTimer>
#include <QQuickWindow>
#include <QImage>
#include <QQmlContext>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include "app_controller.h"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("FileSearcher");
    app.setOrganizationName("FileSearcher");
    app.setApplicationVersion("0.1.0");
    QQuickStyle::setStyle("Basic");
    QTemporaryDir testDirectory;
    const int dataOption = app.arguments().indexOf("--data-dir");
    QString dataDirectory = dataOption >= 0 ? app.arguments().value(dataOption + 1) :
        (app.arguments().contains("--smoke-test") || app.arguments().contains("--screenshot") ? testDirectory.path() : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
    if (dataDirectory.isEmpty()) return 2;
    AppController controller(dataDirectory);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("fileApp", &controller);
    const auto queryOption = app.arguments().indexOf("--query");
    engine.rootContext()->setContextProperty("startupQuery",queryOption >= 0 ? app.arguments().value(queryOption+1) : QString());
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("FileSearcher", "Main");
    const int screenshot = app.arguments().indexOf("--screenshot");
    if (screenshot >= 0) {
        const auto path = app.arguments().value(screenshot + 1);
        auto* capture=new QTimer(&app); capture->setInterval(100);
        auto elapsed=std::make_shared<QElapsedTimer>(); elapsed->start();
        QObject::connect(capture,&QTimer::timeout,&app,[&engine,&controller,capture,elapsed,path] {
            if (elapsed->elapsed()>60000) { QCoreApplication::exit(2); return; }
            if (elapsed->elapsed()<1000 || controller.busy() || controller.searching() || controller.previewLoading()) return;
            capture->stop();
            auto* window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            QCoreApplication::exit(window && !path.isEmpty() && window->grabWindow().save(path) ? 0 : 1);
        });
        capture->start();
    } else if (app.arguments().contains("--smoke-test")) {
        QTimer::singleShot(1000, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
