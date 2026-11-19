#include "app/app_controller.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QTest>
#include <QImage>
#include <QTextStream>
#include <stdexcept>

void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
void waitFor(const std::function<bool()>& condition) {
    QElapsedTimer elapsed; elapsed.start();
    while(elapsed.elapsed()<10000) { QCoreApplication::processEvents(); if(condition()) return; QTest::qWait(5); }
    throw std::runtime_error("UI condition timed out");
}
int main(int argc,char** argv) {
    QGuiApplication app(argc,argv); QQuickStyle::setStyle("Basic");
    try {
        require(argc==3,"Expected QML file and screenshot directory");
        QTemporaryDir temp; const auto root=temp.path()+"/documents"; QDir().mkpath(root);
        for(int i=0;i<8;++i) { QFile file(root+QString("/report-%1.txt").arg(i)); require(file.open(QIODevice::WriteOnly),"Cannot create fixture"); file.write("Synthetic keyboard navigation fixture."); }
        AppController controller(temp.path()+"/index",nullptr,{10,10,300000,false,false});
        controller.addFolder(QUrl::fromLocalFile(root),"");
        waitFor([&]{return !controller.busy() && !controller.searching() && !controller.pending();});
        QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("fileApp",&controller); engine.rootContext()->setContextProperty("startupQuery",QString());
        engine.load(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
        require(!engine.rootObjects().isEmpty(),"QML failed to load");
        auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first()); require(window,"Missing window");
        auto* input=window->findChild<QQuickItem*>("searchInput"); auto* results=window->findChild<QQuickItem*>("resultsList");
        auto* storage=window->findChild<QObject*>("storageDialog"); require(input && results && storage,"Missing named UI controls");
        window->requestActivate(); QTest::qWait(250); input->forceActiveFocus();
        for(char letter:QByteArray("report")) QTest::keyClick(window,letter);
        QTest::keyClick(window,Qt::Key_Return);
        waitFor([&]{return !controller.searching() && controller.total()==8;});
        QTest::keyClick(window,Qt::Key_Down); require(results->hasActiveFocus(),"Down did not focus results");
        QTest::keyClick(window,Qt::Key_End); require(results->property("currentIndex").toInt()==7,"End did not select last result");
        QTest::keyClick(window,Qt::Key_Home); require(results->property("currentIndex").toInt()==0,"Home did not select first result");
        QTest::keyClick(window,Qt::Key_PageDown); require(results->property("currentIndex").toInt()==5,"PageDown selection failed");
        QTest::keyClick(window,Qt::Key_PageUp); require(results->property("currentIndex").toInt()==0,"PageUp selection failed");
        QTest::keyClick(window,Qt::Key_Down); const auto selected=window->property("selectedFileId");
        controller.search("report","",0,"","",0,5); waitFor([&]{return !controller.searching();});
        require(window->property("selectedFileId")==selected,"Result refresh lost selection");
        QTest::keyClick(window,Qt::Key_Escape); require(input->hasActiveFocus(),"Escape did not focus search");
        QTest::keyClick(window,Qt::Key_Escape); waitFor([&]{return !controller.searching();}); require(input->property("text").toString().isEmpty(),"Escape did not clear query");
        results->forceActiveFocus(); QTest::keyClick(window,Qt::Key_F,Qt::ControlModifier); require(input->hasActiveFocus(),"Ctrl+F did not focus search");
        const auto screenshots=QString::fromLocal8Bit(argv[2]); QDir().mkpath(screenshots);
        QTest::qWait(200); require(window->grabWindow().save(screenshots+"/phase7-main.png"),"Cannot save screenshot");
        window->resize(1000,650); QTest::qWait(200);
        require(results->height()>=120,"Minimum window leaves too little room for results");
        require(window->grabWindow().save(screenshots+"/phase7-minimum.png"),"Cannot save minimum screenshot");
        QMetaObject::invokeMethod(storage,"open"); QTest::qWait(250); require(storage->property("visible").toBool(),"Storage dialog did not open");
        require(window->grabWindow().save(screenshots+"/phase7-storage.png"),"Cannot save storage screenshot");
        require(storage->property("height").toDouble()<=window->height(),"Storage dialog exceeds minimum window height");
        QMetaObject::invokeMethod(storage,"close"); QTest::qWait(200);
        auto* organizer=qobject_cast<OrganizationController*>(controller.organizer()); require(organizer,"Missing organization controller");
        waitFor([&]{return !organizer->busy();});
        organizer->inspect(controller.results().first().toMap()["fileId"].toLongLong()); waitFor([&]{return !organizer->busy();});
        require(!organizer->selected().isEmpty(),"Cannot inspect indexed file");
        auto* panel=window->findChild<QObject*>("organizationPanel"); require(panel,"Missing organization panel");
        QMetaObject::invokeMethod(panel,"open"); QTest::qWait(250);
        require(panel->property("visible").toBool(),"Organization panel failed to open");
        require(window->grabWindow().save(screenshots+"/phase8-related.png"),"Cannot save organization screenshot");
        QMetaObject::invokeMethod(panel,"editCategory",Q_ARG(QVariant,QVariant())); QTest::qWait(200);
        auto* editor=panel->findChild<QObject*>("categoryEditor"); auto* categoryName=panel->findChild<QObject*>("categoryName");
        require(editor && categoryName && editor->property("visible").toBool(),"Category editor did not open");
        categoryName->setProperty("text","Study collection"); QMetaObject::invokeMethod(editor,"accept");
        waitFor([&]{return !organizer->busy();}); require(organizer->categories().size()==1,"Category editor did not save a virtual category");
        auto* tabs=panel->findChild<QObject*>("organizationTabs"); require(tabs,"Missing organization tabs"); tabs->setProperty("currentIndex",1);
        QTest::qWait(200); require(window->grabWindow().save(screenshots+"/phase8-categories.png"),"Cannot save category screenshot");
        const auto destination=temp.path()+"/organized"; require(QDir().mkpath(destination),"Cannot make move destination fixture");
        organizer->prepareMove(destination); waitFor([&]{return !organizer->busy();});
        auto* confirmation=panel->findChild<QObject*>("moveConfirmation"); require(confirmation && confirmation->property("visible").toBool(),"Move review did not open");
        QTest::qWait(200); require(window->grabWindow().save(screenshots+"/phase8-move-review.png"),"Cannot save move review screenshot");
        QMetaObject::invokeMethod(confirmation,"reject"); waitFor([&]{return !organizer->busy();});
        require(QDir(root).entryList(QDir::Files).size()==8 && QDir(destination).entryList(QDir::Files).isEmpty(),"Rejecting a move changed files");
        QMetaObject::invokeMethod(panel,"close");
        auto* cleanup=qobject_cast<CleanupController*>(controller.cleanup()); require(cleanup,"Missing cleanup controller"); waitFor([&]{return !cleanup->busy();});
        auto* cleanupPanel=window->findChild<QObject*>("cleanupPanel"); require(cleanupPanel,"Missing cleanup panel");
        QMetaObject::invokeMethod(cleanupPanel,"open");
        cleanup->scan(controller.roots().first().toMap()["rootId"].toLongLong(),false,false); waitFor([&]{return !cleanup->busy();});
        require(cleanup->candidates().size()==7,"Expected seven separately verified copies");
        QTest::qWait(250); require(window->grabWindow().save(screenshots+"/phase9-duplicates.png"),"Cannot save cleanup screenshot");
        cleanup->prepare(cleanup->candidates().first().toMap()["token"].toString()); waitFor([&]{return !cleanup->busy();});
        auto* cleanupReview=cleanupPanel->findChild<QObject*>("cleanupConfirmation"); require(cleanupReview && cleanupReview->property("visible").toBool(),"Cleanup review did not open");
        require(cleanupReview->property("height").toDouble()<=window->height(),"Cleanup review exceeds minimum window height");
        QTest::qWait(250); require(window->grabWindow().save(screenshots+"/phase9-recycle-review.png"),"Cannot save recycling review screenshot");
        QMetaObject::invokeMethod(cleanupReview,"reject"); waitFor([&]{return !cleanup->busy();});
        require(QDir(root).entryList(QDir::Files).size()==8,"Rejecting cleanup changed files");
        QTextStream(stdout)<<"Keyboard, selection, clear, focus, storage and minimum-size UI checks passed.\n"; return 0;
    } catch(const std::exception& e) { QTextStream(stderr)<<e.what()<<Qt::endl; return 1; }
}
