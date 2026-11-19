#pragma once
#include <QObject>
#include <QVariantList>
#include <QThreadPool>
#include <atomic>
#include <memory>
#include <functional>
#include <QUrl>
class OrganizationController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QVariantList categories READ categories NOTIFY changed)
    Q_PROPERTY(QVariantList related READ related NOTIFY changed)
    Q_PROPERTY(QVariantList suggestions READ suggestions NOTIFY changed)
    Q_PROPERTY(QVariantList history READ history NOTIFY changed)
    Q_PROPERTY(QVariantList members READ members NOTIFY changed)
    Q_PROPERTY(QVariantMap selected READ selected NOTIFY changed)
    Q_PROPERTY(QVariantMap movePlan READ movePlan NOTIFY changed)
public:
    OrganizationController(QString dataDirectory,QObject* parent=nullptr);
    ~OrganizationController() override;
    bool busy() const { return busy_; }
    void setAiEnabled(bool enabled);
    QString message() const { return message_; }
    QVariantList categories() const { return categories_; }
    QVariantList related() const { return related_; }
    QVariantList suggestions() const { return suggestions_; }
    QVariantList history() const { return history_; }
    QVariantList members() const { return members_; }
    QVariantMap selected() const { return selected_; }
    QVariantMap movePlan() const { return movePlan_; }
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void inspect(qint64 file);
    Q_INVOKABLE void saveCategory(qint64 id,const QString& name,const QString& description,const QString& destination);
    Q_INVOKABLE void removeCategory(qint64 id);
    Q_INVOKABLE void addExample(qint64 category);
    Q_INVOKABLE void decide(qint64 category,bool accept);
    Q_INVOKABLE void showMembers(qint64 category);
    Q_INVOKABLE void prepareMove(const QString& destination);
    Q_INVOKABLE void prepareUndo(const QString& id);
    Q_INVOKABLE void reviewMove(const QString& id);
    Q_INVOKABLE void approveMove(const QString& id);
    Q_INVOKABLE void cancelMove(const QString& id);
    Q_INVOKABLE QString localPath(const QUrl& url) const { return url.toLocalFile(); }
    Q_INVOKABLE void clearExamples(qint64 category);
    Q_INVOKABLE void removeMember(qint64 category,const QString& path);
signals:
    void changed();
    void movePlanReady();
    void filesChanged();
private:
    void run(const QString& status,std::function<QVariantMap()> work);
    QString directory_,database_,metadata_,message_;
    QStringList protected_;
    bool busy_=false;
    bool useSemantics_=true;
    QVariantList categories_,related_,suggestions_,history_,members_;
    QVariantMap selected_,movePlan_;
    QThreadPool pool_;
    std::shared_ptr<std::atomic_bool> cancelled_=std::make_shared<std::atomic_bool>(false);
};
