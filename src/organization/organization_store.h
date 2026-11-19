#pragma once
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariantList>
#include <QJsonObject>

namespace filesearcher {
// Separate from the disposable search index: user collections and move history survive rebuilds.
// A connection belongs to one worker thread.
class OrganizationStore {
public:
    explicit OrganizationStore(const QString& database);
    ~OrganizationStore();
    OrganizationStore(const OrganizationStore&)=delete;
    QSqlQuery query(const QString& sql,const QVariantList& values={}) const;
    QVariantList categories() const;
    qint64 saveCategory(qint64 id,const QString& name,const QString& description,const QString& destination);
    void removeCategory(qint64 id);
    void addExample(qint64 id,const QString& path);
    void decide(qint64 category,const QString& path,qint64 size,qint64 modified,const QString& decision);
    QString decision(qint64 category,const QString& path,qint64 size,qint64 modified) const;
    QVariantList members(qint64 category) const;
    QVariantList history() const;
    QJsonObject operation(const QString& id) const;
    void putOperation(const QJsonObject& operation);
    void complete(QJsonObject operation);
private:
    QString connection_;
    QSqlDatabase db_;
};
}
