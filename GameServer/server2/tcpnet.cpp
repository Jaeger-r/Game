#include "tcpnet.h"
#include "packdef.h"
#include "kcpnet.h"
#include <QRandomGenerator>
#include <QSet>
#include "qkcpsocket.h"
#include <QTcpSocket>


///逻辑线程池
class LogicTask : public QRunnable
{
public:
    LogicTask(quint64 cid, QByteArray d)
        : clientId(cid), data(std::move(d)) {}

    void run() override
    {
        //qDebug() << "LogicTask running in thread:" << QThread::currentThread()<< "clientId:" << clientId;
        core::getKernel()->dealData(clientId, data);
    }

private:
    quint64 clientId;
    QByteArray data;
};

TCPNet::TCPNet(QObject* parent)
    : INet (parent),
    server(new QTcpServer(this)),
    m_pKcpNet(nullptr)
{
    connect(server, &QTcpServer::newConnection,
            this, &TCPNet::onNewConnection);

    QThreadPool::globalInstance()->setMaxThreadCount(QThread::idealThreadCount() * 2);

    heartbeatTimer = new QTimer();
    connect(heartbeatTimer, &QTimer::timeout,
            this, &TCPNet::checkHeartbeat);
    heartbeatTimer->start(50000); // 每 5 秒扫描
}


bool TCPNet::initNetWork(const QString& ip, quint16 port)
{
    if (!server->listen(QHostAddress(ip), port))
    {
        qDebug() << "TCPServer Start Failed:" << server->errorString();
        return false;
    }
    else
    {
        qDebug() << "TCPServer Listening on:" << ip << port;
    }

    // 逻辑层 → 网络层（回包）
    if (core::getKernel())
    {
        connect((core*)(core::getKernel()),
                &core::sendToClient,
                this, &TCPNet::sendData,
                Qt::QueuedConnection);
    }
    else
    {
        qDebug()<<"core isnt init";
    }
    return true;
}

void TCPNet::unInitNetWork()
{
    if (server) server->close();

    for (auto it = idToClientInfo.begin(); it != idToClientInfo.end(); ++it)
    {
        QTcpSocket* socket = it.value().socket;
        if (!socket) continue;

        socket->disconnect(this);          // 断开所有信号
        socket->disconnectFromHost();      // 关闭 TCP
        socket->deleteLater();             // Qt 安全删除
    }

    idToClientInfo.clear();
    socketToId.clear();
}

void TCPNet::onNewConnection()
{
    QTcpSocket* socket = server->nextPendingConnection();
    if (!socket) return;

    quint64 clientId = allocClientId();
    clientIds.append(clientId);
    socketToId[socket] = clientId;
    idToClientInfo[clientId].socket = socket;
    idToClientInfo[clientId].lastHeartbeat = QDateTime::currentMSecsSinceEpoch();

    connect(socket,
            &QTcpSocket::readyRead,
            this,
            &TCPNet::onReadyRead);
    connect(socket,
            &QTcpSocket::disconnected,
            this,
            &TCPNet::onClientDisconnected);


    qDebug() << "TCP Client Connected:" << clientId;
}

void TCPNet::onReadyRead()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    quint64 clientId = socketToId.value(socket, 0);

    recvBuffers[clientId].append(socket->readAll());
    QByteArray& buffer = recvBuffers[clientId];

    while (true)
    {
        if (buffer.size() < sizeof(PacketHeader)) return;

        PacketHeader header;
        memcpy(&header, buffer.constData(), sizeof(header));

        if (header.length < sizeof(PacketHeader) || header.length > (64 * 1024))
        {
            qDebug() << "Invalid packet, disconnect client";
            socket->disconnectFromHost();
            return;
        }

        if (buffer.size() < header.length) return;

        QByteArray packet = buffer.left(header.length);
        buffer.remove(0, header.length);

        if (header.cmd == _default_protocol_kcp_negotiate_rq)
        {
            // 请求KCP连接，通过KcpNet处理
            STRU_KCP_NEGOTIATE_RS kcp_negotiate_rs;
            kcp_negotiate_rs.result = 0;  // 默认失败

            if (m_pKcpNet) {
                quint32 kcpConv = 0;
                quint16 kcpPort = 0;

                // 调用KcpNet处理KCP协商
                if (m_pKcpNet->handleKcpNegotiate(clientId, socket, kcpConv, kcpPort)) {
                    kcp_negotiate_rs.result = 1;
                    kcp_negotiate_rs.kcpConv = kcpConv;
                    kcp_negotiate_rs.kcpMode = 2;  // Fast模式
                    kcp_negotiate_rs.kcpPort = kcpPort;

                    qDebug() << "KCP Negotiate Success for Client:" << clientId
                             << "Conv:" << kcpConv << "Port:" << kcpPort;
                } else {
                    qDebug() << "KCP Negotiate Failed for Client:" << clientId;
                }
            } else {
                qDebug() << "KCP Negotiate Failed: KcpNet not initialized for Client:" << clientId;
            }

            QByteArray packet = PacketBuilder::build(_default_protocol_kcp_negotiate_rs, kcp_negotiate_rs);
            sendData(clientId, packet);
        }
        else if (header.cmd == _default_protocol_heartbeat_rq)
        {
            idToClientInfo[clientId].lastHeartbeat = QDateTime::currentMSecsSinceEpoch();

            // 立刻回应
            STRU_HEARTBEAT_RS heartbeat_rs;
            QByteArray resp = PacketBuilder::build(_default_protocol_heartbeat_rs, heartbeat_rs);
            sendData(clientId, resp);
        }
        else
        {
            // 投递到逻辑线程池
            LogicTask* task = new LogicTask(clientId, packet);
            task->setAutoDelete(true);
            QThreadPool::globalInstance()->start(task);
        }
    }
}

void TCPNet::checkHeartbeat()
{
    JaegerDebug();
    qint64 now = QDateTime::currentMSecsSinceEpoch();

    QList<quint64> toKick;

    for (auto it = idToClientInfo.begin(); it != idToClientInfo.end(); ++it) {
        if (now - it.value().lastHeartbeat > 15000) {
            toKick.append(it.key());
        }
    }

    for (quint64 clientId : toKick) {
        qDebug() << "Heartbeat timeout, kick client:" << clientId;
        kickClient(clientId);
    }
}

void TCPNet::kickClient(quint64 clientId)
{
    JaegerDebug();
    auto it = idToClientInfo.find(clientId);
    if (it == idToClientInfo.end()) return;

    QTcpSocket* socket = it.value().socket;

    if (clientIds.contains(clientId)) {clientIds.removeOne(clientId);}
    if (socketToId.contains(socket)) {socketToId.remove(socket);}
    if (idToClientInfo.contains(clientId)) {idToClientInfo.remove(clientId);}
    if (recvBuffers.contains(clientId)) {recvBuffers.remove(clientId);}

    recycleClientId(clientId);


    socket->disconnectFromHost();
    socket->deleteLater();
}

void TCPNet::onClientDisconnected()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    quint64 clientId = socketToId.value(socket, 0);
    if (recvBuffers.contains(clientId)) {recvBuffers.remove(clientId);}

    auto it = socketToId.find(socket);
    if (it != socketToId.end())
    {
        quint64 clientId = it.value();
        if (clientIds.contains(clientId)) {clientIds.removeOne(clientId);}
        if (socketToId.contains(socket)) {socketToId.erase(it);}
        if (idToClientInfo.contains(clientId)) {idToClientInfo.remove(clientId);}
        recycleClientId(clientId);
        qDebug() << "TCP Client Disconnected:" << clientId;

        // 清理对应的KCP socket
        if (m_pKcpNet) {
            m_pKcpNet->removeKcpSocket(clientId);
        }
    }

    socket->deleteLater();
}

//回包（主线程）
void TCPNet::sendData(quint64 clientId, QByteArray data)
{
    //JaegerDebug();
    auto it = idToClientInfo.find(clientId);
    if (it == idToClientInfo.end()) return;

    QTcpSocket* socket = it.value().socket;
    if (socket->state() == QAbstractSocket::ConnectedState) socket->write(data);
}
