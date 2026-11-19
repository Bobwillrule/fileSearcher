#pragma once
#include <QObject>
#include <QThreadPool>
#include <QVariantList>
#include <atomic>
#include <functional>
class CleanupController:public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QVariantList candidates READ candidates NOTIFY changed)
    Q_PROPERTY(QVariantList related READ related NOTIFY changed)
    Q_PROPERTY(QVariantList history READ history NOTIFY changed)
    Q_PROPERTY(QVariantMap plan READ plan NOTIFY changed)
public:
    explicit CleanupController(QString directory,QObject* parent=nullptr);
    ~CleanupController() override;
    bool busy() const { return busy_; }
    QString message() const { return message_; }
    QVariantList candidates() const { return candidates_; }
    QVariantList related() const { return related_; }
    QVariantList history() const { return history_; }
    QVariantMap plan() const { return plan_; }
    void setAiEnabled(bool enabled);
    Q_INVOKABLE void scan(qint64 root,bool installers,bool builds);
    Q_INVOKABLE void inspect(qint64 file);
    Q_INVOKABLE void prepare(const QString& token,bool reverse=false);
    Q_INVOKABLE void approve(const QString& id);
    Q_INVOKABLE void dismiss(const QString& id);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void refresh();
signals:
    void changed();
    void reviewReady();
    void filesChanged();
private:
    void run(const QString& message,std::function<QVariantMap()> work);
    QString directory_,database_,message_="Choose a folder and scan. No files are selected for recycling automatically.";
    QStringList protected_;
    bool busy_=false,ai_=true;
    QVariantList candidates_,related_,history_;
    QVariantMap plan_;
    QThreadPool pool_;
    std::atomic_bool cancelled_=false;
};
