#ifndef SERVERMONITORBRIDGE_H
#define SERVERMONITORBRIDGE_H

#include <QObject>
#include <QStringList>
#include <QMutex>

class ServerMonitorBridge : public QObject
{
    Q_OBJECT
public:
    static ServerMonitorBridge* instance();

    QStringList recentLogs() const;
    void appendLogLine(const QString& line);

signals:
    void logAdded(const QString& line);

private:
    explicit ServerMonitorBridge(QObject* parent = nullptr);

    mutable QMutex m_mutex;
    QStringList m_recentLogs;
    static constexpr int kMaxLogLines = 300;
};

#endif // SERVERMONITORBRIDGE_H
