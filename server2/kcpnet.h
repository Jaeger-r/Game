#ifndef KCPNET_H
#define KCPNET_H

#include <QMap>
#include <QHash>
#include <QDebug>
#include <QHostAddress>
#include <string.h>
#include "INet.h"
#include <QUdpSocket>
#include "qkcpserver.h"
#include "servermonitortypes.h"

// 前向声明
QT_FORWARD_DECLARE_CLASS(QKcpSocket);
QT_FORWARD_DECLARE_CLASS(core);
QT_FORWARD_DECLARE_CLASS(QTcpSocket);

class KcpNet : public INet {
    Q_OBJECT
public:
    explicit KcpNet(QObject* parent = nullptr);

    // 实现INet接口（但实际不使用，连接由TCP管理）
    bool initNetWork(const QString& szip = SERVER_IP_LOCATION, quint16 sport = KCP_PORT_REALTIME_MOVE) override;
    void unInitNetWork() override;

    /**
     * @brief 处理KCP协商请求，创建KCP连接
     * @param clientId 客户端ID（与TCP连接的clientId相同）
     * @param tcpSocket TCP socket，用于获取客户端地址
     * @param kcpConv 输出的KCP会话ID
     * @param kcpPort 输出的KCP UDP端口
     * @return 是否创建成功
     */
    bool handleKcpNegotiate(quint64 clientId, QTcpSocket* tcpSocket, quint32& kcpConv, quint16& kcpPort);

    /**
     * @brief 为指定客户端创建KCP socket（内部使用）
     * @param clientId 客户端ID
     * @param peerAddress 客户端地址
     * @param peerPort 客户端端口
     * @param kcpConv KCP会话ID
     * @return 是否创建成功
     */
    bool createKcpSocket(quint64 clientId, const QHostAddress& peerAddress, quint16 peerPort, quint32 kcpConv);

    /**
     * @brief 移除指定客户端的KCP socket
     * @param clientId 客户端ID
     */
    void removeKcpSocket(quint64 clientId);

    /**
     * @brief 获取指定客户端的KCP会话ID
     * @param clientId 客户端ID
     * @return KCP会话ID，如果不存在返回0
     */
    quint32 getKcpConv(quint64 clientId) const;

    /**
     * @brief 获取指定客户端的KCP地址
     * @param clientId 客户端ID
     * @return 客户端地址，如果不存在返回QHostAddress()
     */
    QHostAddress getKcpAddress(quint64 clientId) const;

    /**
     * @brief 获取指定客户端的KCP端口
     * @param clientId 客户端ID
     * @return 客户端端口，如果不存在返回0
     */
    quint16 getKcpPort(quint64 clientId) const;

    /**
     * @brief 发送数据到指定客户端（通过KCP）
     * @param clientId 客户端ID
     * @param data 要发送的数据
     * @return 是否发送成功
     */
    bool sendData(quint64 clientId, const QByteArray& data);
    bool isKcpConnected(quint64 clientId) const;
    KcpMonitorStats monitorStats() const;
    QTcpSocket* getSocket()  { return nullptr; } // KCP不使用TCP Socket数据

    /**
     * @brief 处理从UDP接收到的KCP数据
     * @param data UDP数据包
     * @param peerAddress 发送方地址
     * @param peerPort 发送方端口
     */
    void onUdpDataReceived(const QByteArray& data, const QHostAddress& peerAddress, quint16 peerPort);

signals:
    /**
     * @brief KCP数据接收信号
     * @param clientId 客户端ID
     * @param data 接收到的数据
     */
    void kcpDataReceived(quint64 clientId, const QByteArray& data);

private slots:
    void onNewConnection();
    void onKcpSocketReadyRead();

private:
    QKcpServer* kcpserver;

    // 客户端ID到KCP socket的映射
    QHash<quint64, QKcpSocket*> idToKcpSocket;

    // KCP socket到客户端ID的映射（用于UDP数据包路由）
    QHash<QPair<QHostAddress, quint16>, quint64> addressToClientId;

    // conv会话ID到客户端ID的映射（用于多路复用识别）
    QHash<quint32, quint64> convToClientId;

    // 客户端ID到KCP会话信息的映射（管理conv、address、port）
    struct KcpClientInfo {
        quint32 conv;              // KCP会话ID
        QHostAddress address;        // 客户端地址
        quint16 port;               // 客户端端口
        QKcpSocket* socket;        // KCP socket指针
    };
    QHash<quint64, KcpClientInfo> kcpClientInfo;  // 客户端ID到KCP信息的映射
    bool m_listening = false;
    quint16 m_listenPort = 0;

    void updateKcpSockets();
};

#endif // KCPNET_H
