#include "app/app_controller.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QTextStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <stdexcept>

namespace {
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
void waitFor(const std::function<bool()>& ready) {
    QElapsedTimer timer; timer.start();
    while(timer.elapsed()<120000) { QCoreApplication::processEvents(); if(ready()) return; QThread::msleep(2); }
    throw std::runtime_error("Timed out waiting for the application");
}
void save(const QString& path,const QByteArray& bytes) {
    QFile file(path); require(file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size(),"Cannot create smoke fixture");
}
bool done(const AppController& controller) { return !controller.busy() && !controller.searching() && controller.pending()==0; }
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); app.setApplicationName("FileSearcherPackageCheck");
    QTextStream out(stdout);
    try {
        const bool scale=app.arguments().contains("--scale");
        QTemporaryDir temp; require(temp.isValid(),"Cannot create temporary test directory");
        const auto root=temp.path()+"/documents",data=temp.path()+"/index";
        require(QDir().mkpath(root),"Cannot create fixture directory");
        const QByteArray original="A semiconductor diode conducts electricity in one direction and blocks reverse current. Rectifiers convert alternating current to direct current.";
        save(root+"/diodes.txt",original);
        if(scale) {
            for(int i=0;i<1000;++i) save(root+QString("/small-%1.txt").arg(i),original+QByteArray::number(i));
            for(int i=0;i<4;++i) save(root+QString("/large-%1.txt").arg(i),original.repeated(2000));
        } else {
            const auto fixtures=QCoreApplication::applicationDirPath()+"/smoke-fixtures";
            for(const auto& name:{"reference.pdf","reference.docx"})
                require(QFile::copy(fixtures+"/"+name,root+"/"+name),"Missing packaged PDF/DOCX fixture");
        }
        QJsonObject report{{"physical_small_files",scale?1001:1},{"physical_large_documents",scale?4:2},{"ai_enabled",!scale}};
        {
            AppController controller(data,nullptr,{30,10,300000,true,!scale});
            QElapsedTimer elapsed,gap; elapsed.start(); gap.start(); qint64 maxGap=0; int ticks=0;
            QTimer heartbeat; QObject::connect(&heartbeat,&QTimer::timeout,[&] {maxGap=std::max(maxGap,gap.restart()); ++ticks;}); heartbeat.start(10);
            controller.addFolder(QUrl::fromLocalFile(root),""); waitFor([&]{return done(controller);});
            require(controller.error().isEmpty(),qPrintable(controller.error()));
            report["index_ms"]=elapsed.elapsed(); report["ui_heartbeat_ticks"]=ticks; report["max_heartbeat_gap_ms"]=maxGap;
            require(controller.total()==(scale?1005:3),"Incorrect indexed file count");
            if(!scale) {
                for(const auto& result:controller.results()) {
                    const auto row=result.toMap();
                    require(row["contentStatus"]=="ok",qPrintable("Packaged reader failed: "+row["name"].toString()+" / "+row["contentStatus"].toString()));
                }
                require(controller.aiStatus().contains("Local AI ready"),"Local AI did not finish");
                for(int i=0;i<2;++i) {
                    controller.search("Which component lets electricity pass in just one direction?","",0,"","",0,5);
                    waitFor([&]{return !controller.searching();});
                    require(!controller.results().isEmpty() && controller.results().first().toMap()["name"]=="diodes.txt","Semantic search did not find the diode document");
                    require(controller.results().first().toMap()["matchLabel"].toString().contains("Similar meaning"),"Semantic evidence missing");
                }
                const auto fileId=controller.results().first().toMap()["fileId"].toLongLong();
                auto* organizer=qobject_cast<OrganizationController*>(controller.organizer()); require(organizer,"Organization controller missing");
                waitFor([&]{return !organizer->busy();});
                const auto destination=temp.path()+"/organized"; require(QDir().mkpath(destination),"Cannot create move destination");
                organizer->saveCategory(0,"Electronics","semiconductor diode rectifier",destination); waitFor([&]{return !organizer->busy();});
                organizer->inspect(fileId); waitFor([&]{return !organizer->busy();}); require(!organizer->suggestions().isEmpty(),"Category evidence unavailable");
                organizer->decide(organizer->categories().first().toMap()["id"].toLongLong(),true); waitFor([&]{return !organizer->busy();}); require(QFileInfo::exists(root+"/diodes.txt"),"Tag acceptance moved a file");
                organizer->prepareMove(destination); waitFor([&]{return !organizer->busy();}); require(!organizer->movePlan().isEmpty(),"Move review unavailable");
                const auto moveId=organizer->movePlan()["id"].toString(); organizer->approveMove(moveId); waitFor([&]{return !organizer->busy() && done(controller);});
                require(QFileInfo::exists(destination+"/diodes.txt") && !QFileInfo::exists(root+"/diodes.txt"),"Approved move failed");
                organizer->prepareUndo(moveId); waitFor([&]{return !organizer->busy();}); require(!organizer->movePlan().isEmpty(),"Undo review unavailable");
                organizer->approveMove(organizer->movePlan()["id"].toString()); waitFor([&]{return !organizer->busy() && done(controller);});
                require(QFileInfo::exists(root+"/diodes.txt") && !QFileInfo::exists(destination+"/diodes.txt"),"Approved undo failed");
                report["category_tag_move_undo"]=true;
                const auto duplicate=root+"/z-diodes-copy.txt";
                require(QFile::copy(root+"/diodes.txt",duplicate),"Cannot create synthetic duplicate"); controller.scan(); waitFor([&]{return done(controller);});
                auto* cleanup=qobject_cast<CleanupController*>(controller.cleanup()); require(cleanup,"Cleanup controller unavailable"); waitFor([&]{return !cleanup->busy();});
                cleanup->scan(controller.roots().first().toMap()["rootId"].toLongLong(),false,false); waitFor([&]{return !cleanup->busy();});
                require(cleanup->candidates().size()==1,"Exact duplicate not discovered");
                cleanup->prepare(cleanup->candidates().first().toMap()["token"].toString()); waitFor([&]{return !cleanup->busy();}); require(!cleanup->plan().isEmpty(),"Cleanup review unavailable");
                require(cleanup->plan()["path"].toString()==duplicate,"Cleanup selected unexpected file");
                cleanup->approve(cleanup->plan()["id"].toString()); waitFor([&]{return !cleanup->busy() && done(controller);});
                require(!QFileInfo::exists(duplicate) && QFileInfo::exists(root+"/diodes.txt"),"Verified duplicate was not recycled");
                require(cleanup->history().first().toMap()["state"]=="recycled","Recycle Bin did not confirm completion");
                report["duplicate_recycle_retains_original"]=true;
            }
            controller.refreshStorage(); waitFor([&]{return controller.storageInfo().contains("files");});
            report["passages"]=QJsonValue::fromVariant(controller.storageInfo()["passages"]);
        }
        {
            AppController restarted(data,nullptr,{30,10,300000,true,!scale}); waitFor([&]{return done(restarted);});
            require(restarted.total()==(scale?1005:3),"Restart lost indexed files");
            restarted.clearSavedIndex(); waitFor([&]{return !restarted.busy() && !restarted.searching();});
            require(restarted.error().isEmpty() && restarted.total()==0 && restarted.roots().isEmpty(),"Index clear failed");
        }
        QFile originalFile(root+"/diodes.txt"); require(originalFile.open(QIODevice::ReadOnly) && originalFile.readAll()==original,"Original file was changed");
        report["restart_and_clear_preserve_originals"]=true;
        out<<QJsonDocument(report).toJson(); return 0;
    } catch(const std::exception& e) { out<<e.what()<<Qt::endl; return 1; }
}
