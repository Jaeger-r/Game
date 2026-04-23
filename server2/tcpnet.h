#ifndef TCPSERVER_H
#define TCPSERVER_H

#include <QTcpServer>
#include <QTcpSocket>
#include <QMap>
#include <QHash>
#include <QDateTime>
#include <QTimer>
#include <QDebug>
#include <string.h>
#include <atomic>
#include "INet.h"
#include "core.h"
#include "../servermonitortypes.h"
#include <QVector>
#include <qkcpserver.h>

QT_FORWARD_DECLARE_CLASS(KcpNet);

class TCPNet : public INet {
    Q_OBJECT
public:
    explicit TCPNet(QObject* parent = nullptr);

    bool initNetWork(const QString& ip, quint16 port);
    void unInitNetWork();

    //可回收ID
    quint64 allocClientId() {
        if (!freeIds.empty()) {
            quint64 id = freeIds.front();
            freeIds.pop();
            return id;
        }
        return nextClientId++;
    }
    void recycleClientId(quint64 id) {freeIds.push(id);}

    void checkHeartbeat();
    void kickClient(quint64 clientId);
    void notifyLogicTaskStarted();
    void notifyLogicTaskFinished(quint16 cmd, qint64 elapsedNs);

    // 设置KcpNet指针（由kernel调用）
    void setKcpNet(KcpNet* kcpNet) { m_pKcpNet = kcpNet; }

    //返回客户端数量
    int getClientCount() const
    {
        return idToClientInfo.size();
    }
    //clientIds接口
    const QVector<quint64>& getAllClientIds() const
    {
        return clientIds;
    }
    TcpMonitorStats monitorStats() const;

    QTcpSocket* getSocket(quint64 clientId) {
        if (idToClientInfo.contains(clientId)) {
            return idToClientInfo[clientId].socket;
        }
        return nullptr;
    }
private slots:
    void onNewConnection();
    void onReadyRead();
    void onClientDisconnected();
    void sendData(quint64 clientId, const QByteArray& data);
    void logRuntimeStats();

signals:
    void clientDisconnected(quint64 clientId);

private:

    std::queue<quint64> freeIds;
    std::atomic<quint64> nextClientId{1};

    struct ClientInfo {
        QTcpSocket* socket;
        quint64 lastHeartbeat;
    };

    QTcpServer* server;
    KcpNet* m_pKcpNet;

    QVector<quint64> clientIds;
    QHash<QTcpSocket*, quint64> socketToId;
    QHash<quint64, ClientInfo> idToClientInfo;
    QMap<quint64, QByteArray> recvBuffers;

    QTimer* heartbeatTimer;
    QTimer* monitorTimer = nullptr;
    std::atomic<int> m_logicActiveCount{0};
    std::atomic<quint64> m_totalPacketCount{0};
    std::atomic<quint64> m_logicPacketCount{0};
    std::atomic<quint64> m_logicProcessingTotalNs{0};
    std::atomic<quint64> m_logicProcessingMaxNs{0};
    quint64 m_lastLoggedPacketCount = 0;
    quint64 m_lastLoggedLogicPacketCount = 0;
    quint16 m_listenPort = 0;

};

#endif // TCPSERVER_H
