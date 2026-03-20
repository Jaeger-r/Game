#include "tcpnet.h"
#include "packdef.h"
#include "kcpnet.h"
#include <QRandomGenerator>
#include <QSet>
#include <QElapsedTimer>
#include "qkcpsocket.h"
#include <QTcpSocket>

namespace {
QString packetCommandLabel(quint16 cmd)
{
    switch (cmd) {
    case _default_protocol_heartbeat_rq: return QStringLiteral("heartbeat_rq");
    case _default_protocol_kcp_negotiate_rq: return QStringLiteral("kcp_negotiate_rq");
    case _default_protocol_chat_rq: return QStringLiteral("chat_rq");
    case _default_protocol_location_rq: return QStringLiteral("location_rq");
    case _default_protocol_login_rq: return QStringLiteral("login_rq");
    case _default_protocol_register_rq: return QStringLiteral("register_rq");
    case _default_protocol_initialize_rq: return QStringLiteral("initialize_rq");
    case _default_protocol_save_rq: return QStringLiteral("save_rq");
    case _default_protocol_dazuo_rq: return QStringLiteral("dazuo_rq");
    case _default_protocol_levelup_rq: return QStringLiteral("levelup_rq");
    case _default_protocol_attack_rq: return QStringLiteral("attack_rq");
    case _default_protocol_revive_rq: return QStringLiteral("revive_rq");
    case _default_protocol_initbag_rq: return QStringLiteral("initbag_rq");
    case _default_protocol_playerlist_rq: return QStringLiteral("playerlist_rq");
    default: return QStringLiteral("cmd_%1").arg(cmd);
    }
}
}


///逻辑线程池
class LogicTask : public QRunnable
{
public:
    LogicTask(TCPNet* n, quint64 cid, QByteArray d, quint16 c)
        : net(n), clientId(cid), data(std::move(d)), cmd(c) {}

    void run() override
    {
        if (net) {
            net->notifyLogicTaskStarted();
        }
        QElapsedTimer timer;
        timer.start();
        core::getKernel()->dealData(clientId, data);
        if (net) {
            net->notifyLogicTaskFinished(cmd, timer.nsecsElapsed());
        }
    }

private:
    TCPNet* net;
    quint64 clientId;
    QByteArray data;
    quint16 cmd;
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

    monitorTimer = new QTimer(this);
    connect(monitorTimer, &QTimer::timeout,
            this, &TCPNet::logRuntimeStats);
    monitorTimer->start(5000);
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
        m_totalPacketCount.fetch_add(1, std::memory_order_relaxed);

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
            m_logicQueuedCount.fetch_add(1, std::memory_order_relaxed);
            LogicTask* task = new LogicTask(this, clientId, packet, header.cmd);
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
        if(clientId == 1) {
            qDebug() << "Administrator Id,Do not kick out" << clientId;
        }else {
            qDebug() << "Heartbeat timeout, kick client:" << clientId;
            kickClient(clientId);
        }
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

void TCPNet::notifyLogicTaskStarted()
{
    m_logicQueuedCount.fetch_sub(1, std::memory_order_relaxed);
    m_logicActiveCount.fetch_add(1, std::memory_order_relaxed);
}

void TCPNet::notifyLogicTaskFinished(quint16 cmd, qint64 elapsedNs)
{
    m_logicActiveCount.fetch_sub(1, std::memory_order_relaxed);
    m_logicPacketCount.fetch_add(1, std::memory_order_relaxed);
    m_logicProcessingTotalNs.fetch_add(static_cast<quint64>(qMax<qint64>(0, elapsedNs)),
                                       std::memory_order_relaxed);

    quint64 currentMax = m_logicProcessingMaxNs.load(std::memory_order_relaxed);
    const quint64 safeElapsed = static_cast<quint64>(qMax<qint64>(0, elapsedNs));
    while (safeElapsed > currentMax
           && !m_logicProcessingMaxNs.compare_exchange_weak(currentMax,
                                                            safeElapsed,
                                                            std::memory_order_relaxed)) {
    }

    if (safeElapsed >= 20'000'000ULL) {
        qWarning() << "[Perf] Slow packet"
                   << packetCommandLabel(cmd)
                   << "elapsedMs=" << QString::number(static_cast<double>(safeElapsed) / 1000000.0, 'f', 2)
                   << "queued=" << m_logicQueuedCount.load(std::memory_order_relaxed)
                   << "active=" << m_logicActiveCount.load(std::memory_order_relaxed);
    }
}

void TCPNet::logRuntimeStats()
{
    QThreadPool* pool = QThreadPool::globalInstance();
    const int onlineConnections = idToClientInfo.size();
    const int queued = qMax(0, m_logicQueuedCount.load(std::memory_order_relaxed));
    const int active = qMax(0, m_logicActiveCount.load(std::memory_order_relaxed));
    const quint64 totalPackets = m_totalPacketCount.load(std::memory_order_relaxed);
    const quint64 logicPackets = m_logicPacketCount.load(std::memory_order_relaxed);
    const quint64 totalNs = m_logicProcessingTotalNs.load(std::memory_order_relaxed);
    const quint64 maxNs = m_logicProcessingMaxNs.load(std::memory_order_relaxed);
    const quint64 packetDelta = totalPackets - m_lastLoggedPacketCount;
    const quint64 logicDelta = logicPackets - m_lastLoggedLogicPacketCount;
    m_lastLoggedPacketCount = totalPackets;
    m_lastLoggedLogicPacketCount = logicPackets;

    const double avgLogicMs = logicPackets > 0
                                  ? static_cast<double>(totalNs) / static_cast<double>(logicPackets) / 1000000.0
                                  : 0.0;
    const double maxLogicMs = static_cast<double>(maxNs) / 1000000.0;

    qInfo().noquote()
        << QStringLiteral("[Monitor] online=%1 totalPackets=%2 (+%3/5s) logicPackets=%4 (+%5/5s) "
                          "threadPool(active=%6 queued=%7 maxThreads=%8 qtActive=%9) "
                          "logicAvgMs=%10 logicMaxMs=%11")
              .arg(onlineConnections)
              .arg(totalPackets)
              .arg(packetDelta)
              .arg(logicPackets)
              .arg(logicDelta)
              .arg(active)
              .arg(queued)
              .arg(pool ? pool->maxThreadCount() : 0)
              .arg(pool ? pool->activeThreadCount() : 0)
              .arg(QString::number(avgLogicMs, 'f', 2))
              .arg(QString::number(maxLogicMs, 'f', 2));
}
