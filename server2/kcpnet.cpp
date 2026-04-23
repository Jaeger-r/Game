#include "kcpnet.h"
#include "qkcpsocket.h"
#include <QUdpSocket>
#include <QTimer>
#include <QHostAddress>
#include <QPair>
#include <QMutex>
#include <QSet>
#include <QTcpSocket>
#include <QRandomGenerator>
#include "packdef.h"
#include "core.h"
#include "ikcp.h"

///安全随机唯一KCP会话号
class KcpConvManager {
public:
    /**
     * @brief 处理allocConv相关逻辑
     * @author Jaeger
     * @date 2025.3.28
     */
    static quint32 allocConv()
    {
        QMutexLocker locker(&kcp_conv_mutex);

        quint32 conv;
        do{
            conv = QRandomGenerator::system()->generate();
        }while(conv == 0 || usedConvs.contains(conv));

        usedConvs.insert(conv);
        return conv;
    }
    /**
     * @brief 处理releaseConv相关逻辑
     * @author Jaeger
     * @date 2025.3.28
     */
    static void releaseConv(quint32 conv)
    {
        QMutexLocker locker(&kcp_conv_mutex);
        usedConvs.remove(conv);
    }

private:
    static QSet<quint32> usedConvs;
    static QMutex kcp_conv_mutex;
};
QSet<quint32> KcpConvManager::usedConvs;
QMutex KcpConvManager::kcp_conv_mutex;

/**
 * @brief 构造KcpNet对象并完成基础初始化
 * @author Jaeger
 * @date 2025.3.28
 */
KcpNet::KcpNet(QObject* parent)
    : INet(parent),
    kcpserver(new QKcpServer(this))
{

}

/**
 * @brief 初始化initNetWork相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool KcpNet::initNetWork(const QString& szip, quint16 port)
{
    Q_UNUSED(szip);
    // KCP服务器监听UDP端口
    if (!kcpserver->listen(QHostAddress::Any, port)) {
        qDebug() << "KCP Server listen failed on port:" << port;
        m_listening = false;
        m_listenPort = 0;
        return false;
    }

    m_listening = true;
    m_listenPort = port;

    qDebug() << "KCP Network Layer Initialized on port:" << port << "(Connection managed by TCP)";

    // 逻辑层 → 网络层（KCP回包）
    if (core::getKernel())
    {
        connect((core*)(core::getKernel()),
                &core::sendToClientKcp,
                this, &KcpNet::sendData,
                Qt::DirectConnection);
    }

    // 监听新KCP连接（由客户端主动连接）
    connect(kcpserver, &QKcpServer::newConnection, this, &KcpNet::onNewConnection);

    return true;
}

/**
 * @brief 判断isKcpConnected相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool KcpNet::isKcpConnected(quint64 clientId) const
{
    auto it = idToKcpSocket.find(clientId);
    if (it != idToKcpSocket.end()) {
        return it.value() && it.value()->isConnected();
    }
    return false;
}

/**
 * @brief 查询monitorStats相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
KcpMonitorStats KcpNet::monitorStats() const
{
    KcpMonitorStats stats;
    stats.listening = m_listening;
    stats.port = m_listenPort;
    stats.activeConnections = kcpClientInfo.size();
    return stats;
}

/**
 * @brief 处理unInitNetWork相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void KcpNet::unInitNetWork()
{
    // 清理所有KCP socket
    for (auto it = idToKcpSocket.begin(); it != idToKcpSocket.end(); ++it) {
        if (it.value()) {
            quint32 conv = getKcpConv(it.key());
            if (conv != 0) {
                KcpConvManager::releaseConv(conv);
            }
            it.value()->disconnectFromHost();
            it.value()->deleteLater();
        }
    }
    idToKcpSocket.clear();
    addressToClientId.clear();
    kcpClientInfo.clear();

    if (kcpserver) {
        kcpserver->close();
    }
    m_listening = false;
    m_listenPort = 0;
}

/**
 * @brief 处理handleKcpNegotiate相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool KcpNet::handleKcpNegotiate(quint64 clientId, QTcpSocket* tcpSocket, quint32& kcpConv, quint16& kcpPort)
{
    if (!tcpSocket) {
        qDebug() << "handleKcpNegotiate: tcpSocket is null";
        return false;
    }

    // 分配KCP会话ID
    kcpConv = KcpConvManager::allocConv();
    
    // 记录conv与clientId的对应关系，等待UDP包到来时匹配
    convToClientId[kcpConv] = clientId;

    // 服务器KCP端口（客户端应该连接这个端口）
    kcpPort = KCP_PORT_REALTIME_MOVE;

    qDebug() << "KCP Negotiate Success for Client:" << clientId
             << "Conv:" << kcpConv
             << "Waiting for UDP connection on Server Port:" << kcpPort;

    return true;
}

/**
 * @brief 处理onNewConnection相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void KcpNet::onNewConnection()
{
    while (kcpserver->hasPendingConnections()) {
        QKcpSocket* kcpSocket = kcpserver->nextPendingConnection();
        if (!kcpSocket) continue;

        // 通过 handle 获取 conv ID
        quint32 conv = 0;
        if (kcpSocket->kcpHandle()) {
            conv = ((ikcpcb*)kcpSocket->kcpHandle())->conv;
        }
        qDebug() << "New KCP Connection observed, conv:" << conv;

        if (convToClientId.contains(conv)) {
            quint64 clientId = convToClientId[conv];
            
            // 如果已存在旧的连接，先移除
            if (idToKcpSocket.contains(clientId)) {
                removeKcpSocket(clientId);
            }

            // 建立映射关系
            idToKcpSocket[clientId] = kcpSocket;
            
            KcpClientInfo info;
            info.conv = conv;
            info.address = kcpSocket->peerAddress();
            info.port = kcpSocket->peerPort();
            info.socket = kcpSocket;
            kcpClientInfo[clientId] = info;

            addressToClientId[qMakePair(info.address, info.port)] = clientId;

            // 连接信号
            connect(kcpSocket, &QKcpSocket::readyRead, this, &KcpNet::onKcpSocketReadyRead);
            connect(kcpSocket, &QKcpSocket::disconnected, this, [this, clientId]() {
                qDebug() << "KCP Socket Disconnected for Client:" << clientId;
                removeKcpSocket(clientId);
            });

            qDebug() << "KCP Connection Established for Client:" << clientId << "Conv:" << conv;
        } else {
            qDebug() << "Unknown KCP conv received:" << conv << ", closing.";
            kcpSocket->disconnectFromHost();
            kcpSocket->deleteLater();
        }
    }
}

/**
 * @brief 创建createKcpSocket相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool KcpNet::createKcpSocket(quint64 clientId, const QHostAddress& peerAddress, quint16 peerPort, quint32 kcpConv)
{
    // 创建KCP socket（使用Fast模式以获得最低延迟）
    QKcpSocket* kcpSocket = new QKcpSocket(QKcpSocket::Mode::Fast, kcpConv, this);

    // 连接到对端（使用UDP）
    kcpSocket->connectToHost(peerAddress, peerPort);

    // 连接数据接收信号
    connect(kcpSocket, &QKcpSocket::readyRead, this, &KcpNet::onKcpSocketReadyRead);
    connect(kcpSocket, &QKcpSocket::disconnected, this, [this, clientId]() {
        qDebug() << "KCP Socket Disconnected for Client:" << clientId;
        removeKcpSocket(clientId);
    });

    // 连接建立后，QtKcp会更新实际的address和port（从第一个UDP包获取）
    // 我们需要在连接建立后更新保存的address和port
    connect(kcpSocket, &QKcpSocket::connected, this, [this, clientId, kcpSocket]() {
        // 从QKcpSocket获取实际的address和port（QtKcp在_read_udp_data中会更新）
        QHostAddress actualAddress = kcpSocket->peerAddress();
        quint16 actualPort = kcpSocket->peerPort();

        // 更新KCP客户端信息中的address和port
        auto infoIt = kcpClientInfo.find(clientId);
        if (infoIt != kcpClientInfo.end()) {
            // 如果地址改变了，更新地址映射
            QHostAddress oldAddress = infoIt.value().address;
            quint16 oldPort = infoIt.value().port;

            if (oldAddress != actualAddress || oldPort != actualPort) {
                // 移除旧的地址映射
                addressToClientId.remove(qMakePair(oldAddress, oldPort));

                // 更新为实际的address和port
                infoIt.value().address = actualAddress;
                infoIt.value().port = actualPort;

                // 添加新的地址映射
                addressToClientId[qMakePair(actualAddress, actualPort)] = clientId;

                qDebug() << "KCP Socket Address Updated for Client:" << clientId
                         << "Old:" << oldAddress << ":" << oldPort
                         << "New:" << actualAddress << ":" << actualPort;
            }
        }
    });

    // 保存映射关系
    idToKcpSocket[clientId] = kcpSocket;

    // 保存KCP客户端信息（管理conv、address、port）
    // 注意：初始保存的address和port可能会在连接建立后被QtKcp更新
    // 我们会在connected信号中同步更新
    KcpClientInfo info;
    info.conv = kcpConv;
    info.address = peerAddress;  // 初始地址，可能被QtKcp更新
    info.port = peerPort;         // 初始端口，可能被QtKcp更新
    info.socket = kcpSocket;
    kcpClientInfo[clientId] = info;

    // 保存地址到客户端ID的映射（初始值，连接建立后可能会更新）
    addressToClientId[qMakePair(peerAddress, peerPort)] = clientId;

    qDebug() << "KCP Socket Created for Client:" << clientId
             << "Conv:" << kcpConv
             << "Peer:" << peerAddress << ":" << peerPort;

    return true;
}

/**
 * @brief 处理removeKcpSocket相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void KcpNet::removeKcpSocket(quint64 clientId)
{
    // 从KCP客户端信息中获取conv并释放
    auto infoIt = kcpClientInfo.find(clientId);
    if (infoIt != kcpClientInfo.end()) {
        quint32 conv = infoIt.value().conv;
        KcpConvManager::releaseConv(conv);
        convToClientId.remove(conv); // 移除conv映射
        // 从地址映射中移除
        addressToClientId.remove(qMakePair(infoIt.value().address, infoIt.value().port));
        kcpClientInfo.erase(infoIt);
    }

    // 从socket映射中移除并删除socket
    auto it = idToKcpSocket.find(clientId);
    if (it != idToKcpSocket.end()) {
        QKcpSocket* kcpSocket = it.value();
        kcpSocket->disconnectFromHost();
        kcpSocket->deleteLater();
        idToKcpSocket.erase(it);
    }

    qDebug() << "KCP Socket Removed for Client:" << clientId;
}

/**
 * @brief 获取getKcpConv相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
quint32 KcpNet::getKcpConv(quint64 clientId) const
{
    auto it = kcpClientInfo.find(clientId);
    if (it != kcpClientInfo.end()) {
        return it.value().conv;
    }
    return 0;
}

/**
 * @brief 获取getKcpAddress相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
QHostAddress KcpNet::getKcpAddress(quint64 clientId) const
{
    auto it = kcpClientInfo.find(clientId);
    if (it != kcpClientInfo.end()) {
        return it.value().address;
    }
    return QHostAddress();
}

/**
 * @brief 获取getKcpPort相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
quint16 KcpNet::getKcpPort(quint64 clientId) const
{
    auto it = kcpClientInfo.find(clientId);
    if (it != kcpClientInfo.end()) {
        return it.value().port;
    }
    return 0;
}

/**
 * @brief 发送sendData相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool KcpNet::sendData(quint64 clientId, const QByteArray& data)
{
    auto it = idToKcpSocket.find(clientId);
    if (it == idToKcpSocket.end()) {
        // KCP socket不存在，可能是该客户端未启用KCP或已断开
        return false;
    }

    QKcpSocket* kcpSocket = it.value();
    if (!kcpSocket->isConnected()) {
        return false;
    }

    qint64 written = kcpSocket->write(data);
    return written > 0;
}

/**
 * @brief 处理onUdpDataReceived相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void KcpNet::onUdpDataReceived(const QByteArray& data, const QHostAddress& peerAddress, quint16 peerPort)
{
    Q_UNUSED(data);

    // 这个方法用于处理从共享UDP socket接收的数据
    // 但由于QKcpSocket每个都有自己的UDP socket，这个方法可能不会被使用
    // 保留作为扩展接口

    // 根据地址查找对应的客户端ID
    auto key = qMakePair(peerAddress, peerPort);
    auto it = addressToClientId.find(key);

    if (it == addressToClientId.end()) {
        // 未知的地址，可能是新连接或地址映射未建立
        return;
    }

    quint64 clientId = it.value();

    // 查找对应的KCP socket
    auto kcpIt = idToKcpSocket.find(clientId);
    if (kcpIt == idToKcpSocket.end()) {
        return;
    }

    // 注意：由于QKcpSocket内部有自己的UDP socket处理数据
    // 这里的方法主要用于扩展场景
    // 实际数据接收通过onKcpSocketReadyRead处理
}

/**
 * @brief 处理onKcpSocketReadyRead相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void KcpNet::onKcpSocketReadyRead()
{
    QKcpSocket* kcpSocket = qobject_cast<QKcpSocket*>(sender());
    if (!kcpSocket) {
        return;
    }

    // 查找对应的客户端ID
    quint64 clientId = 0;
    for (auto it = idToKcpSocket.begin(); it != idToKcpSocket.end(); ++it) {
        if (it.value() == kcpSocket) {
            clientId = it.key();
            break;
        }
    }

    if (clientId == 0) {
        return;
    }

    // 读取数据
    const QByteArray data = kcpSocket->readAll();

    if (data.isEmpty()) {
        return;
    }

    // 发送数据接收信号
    emit kcpDataReceived(clientId, data);
}

/**
 * @brief 更新updateKcpSockets相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void KcpNet::updateKcpSockets()
{
    // KCP需要定期更新，但QKcpSocket内部已经有定时器处理
    // 这里可以做一些额外的维护工作
    // 实际上QKcpSocket的timerEvent会处理KCP更新
}
