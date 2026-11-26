#include "storage/state_backup.h"
#include <QCoreApplication>
#include <QTextStream>
#include <QJsonDocument>
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); const auto args=app.arguments();
    try {
        if(args.size()==3 && args[1]=="--verify") { QTextStream(stdout)<<QJsonDocument(filesearcher::verifyStateBackup(args[2])).toJson(); return 0; }
        if(args.size()==5 && args[1]=="--restore" && args[3]=="--destination") { filesearcher::restoreStateBackup(args[2],args[4]); QTextStream(stdout)<<"Restored into the new folder. Existing state and original documents were not changed.\n"; return 0; }
        QTextStream(stderr)<<"Usage: state_backup --verify BACKUP | --restore BACKUP --destination NEW_FOLDER\n"; return 2;
    } catch(const std::exception& e) { QTextStream(stderr)<<e.what()<<Qt::endl; return 1; }
}
