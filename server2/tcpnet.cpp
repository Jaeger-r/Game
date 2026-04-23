#include "tcpnet.h"
#include "packdef.h"
#include "kcpnet.h"
#include <QRandomGenerator>
#include <QSet>
#include <QElapsedTimer>
#include "qkcpsocket.h"
#include <QTcpSocket>

namespace {
constexpr qsizetype kPacketHeaderSize = static_cast<qsizetype>(sizeof(PacketHeader));
constexpr quint16 kMaxPacketBytes = 60 * 1024;
/**
 * @brief 处理packetCommandLabel相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
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


/**
 * @brief 构造TCPNet对象并完成基础初始化
 * @author Jaeger
 * @date 2025.3.28
 */
TCPNet::TCPNet(QObject* parent)
    : INet (parent),
    server(new QTcpServer(this)),
    m_pKcpNet(nullptr)
{
    connect(server, &QTcpServer::newConnection,
            this, &TCPNet::onNewConnection);

    heartbeatTimer = new QTimer();
    connect(heartbeatTimer, &QTimer::timeout,
            this, &TCPNet::checkHeartbeat);
    heartbeatTimer->start(50000); // 每 5 秒扫描

    monitorTimer = new QTimer(this);
    connect(monitorTimer, &QTimer::timeout,
            this, &TCPNet::logRuntimeStats);
    monitorTimer->start(5000);
}


/**
 * @brief 初始化initNetWork相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool TCPNet::initNetWork(const QString& ip, quint16 port)
{
    m_listenPort = port;
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
                Qt::DirectConnection);
    }
    else
    {
        qDebug()<<"core isnt init";
    }
    return true;
}

/**
 * @brief 处理unInitNetWork相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void TCPNet::unInitNetWork()
{
    if (server) server->close();
    m_listenPort = 0;

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

/**
 * @brief 处理onNewConnection相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
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

/**
 * @brief 处理onReadyRead相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void TCPNet::onReadyRead()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    quint64 clientId = socketToId.value(socket, 0);

    recvBuffers[clientId].append(socket->readAll());
    QByteArray& buffer = recvBuffers[clientId];

    while (true)
    {
        if (buffer.size() < kPacketHeaderSize) return;

        PacketHeader header;
        memcpy(&header, buffer.constData(), sizeof(header));

        if (header.length < sizeof(PacketHeader) || header.length > kMaxPacketBytes)
        {
            qDebug() << "Invalid packet, disconnect client";
            socket->disconnectFromHost();
            return;
        }

        if (buffer.size() < header.length) return;

        const QByteArray packet = QByteArray::fromRawData(buffer.constData(), header.length);
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
            notifyLogicTaskStarted();
            QElapsedTimer timer;
            timer.start();
            core::getKernel()->dealData(clientId, packet);
            notifyLogicTaskFinished(header.cmd, timer.nsecsElapsed());
        }

        buffer.remove(0, header.length);
    }
}

/**
 * @brief 处理checkHeartbeat相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
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

/**
 * @brief 处理kickClient相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
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

    if (m_pKcpNet) {
        m_pKcpNet->removeKcpSocket(clientId);
    }

    emit clientDisconnected(clientId);

    socket->disconnectFromHost();
    socket->deleteLater();
}

/**
 * @brief 处理onClientDisconnected相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
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

        emit clientDisconnected(clientId);
    }

    socket->deleteLater();
}

//回包（主线程）
/**
 * @brief 发送sendData相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void TCPNet::sendData(quint64 clientId, const QByteArray& data)
{
    //JaegerDebug();
    auto it = idToClientInfo.find(clientId);
    if (it == idToClientInfo.end()) return;

    QTcpSocket* socket = it.value().socket;
    if (socket->state() == QAbstractSocket::ConnectedState) socket->write(data);
}

/**
 * @brief 处理notifyLogicTaskStarted相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void TCPNet::notifyLogicTaskStarted()
{
    m_logicActiveCount.fetch_add(1, std::memory_order_relaxed);
}

/**
 * @brief 处理notifyLogicTaskFinished相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
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
                   << "active=" << m_logicActiveCount.load(std::memory_order_relaxed);
    }
}

/**
 * @brief 处理logRuntimeStats相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void TCPNet::logRuntimeStats()
{
    const int onlineConnections = idToClientInfo.size();
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
                          "dispatch(active=%6) "
                          "logicAvgMs=%7 logicMaxMs=%8")
              .arg(onlineConnections)
              .arg(totalPackets)
              .arg(packetDelta)
              .arg(logicPackets)
              .arg(logicDelta)
              .arg(active)
              .arg(QString::number(avgLogicMs, 'f', 2))
              .arg(QString::number(maxLogicMs, 'f', 2));
}

/**
 * @brief 查询monitorStats相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
TcpMonitorStats TCPNet::monitorStats() const
{
    TcpMonitorStats stats;
    stats.listening = server && server->isListening();
    stats.port = m_listenPort;
    stats.onlineConnections = idToClientInfo.size();
    stats.logicActive = qMax(0, m_logicActiveCount.load(std::memory_order_relaxed));
    stats.totalPackets = m_totalPacketCount.load(std::memory_order_relaxed);
    stats.logicPackets = m_logicPacketCount.load(std::memory_order_relaxed);

    const quint64 totalNs = m_logicProcessingTotalNs.load(std::memory_order_relaxed);
    const quint64 maxNs = m_logicProcessingMaxNs.load(std::memory_order_relaxed);
    stats.averageLogicMs = stats.logicPackets > 0
                               ? static_cast<double>(totalNs) / static_cast<double>(stats.logicPackets) / 1000000.0
                               : 0.0;
    stats.maxLogicMs = static_cast<double>(maxNs) / 1000000.0;
    return stats;
}
