#ifndef PACKETBUILDER_H
#define PACKETBUILDER_H

#include <QByteArray>
#include "packdef.h"

class PacketBuilder
{
public:
    // 构造无 body 的包（如心跳）
    static QByteArray build(quint16 cmd)
    {
        PacketHeader header;
        header.cmd = cmd;
        header.length = sizeof(PacketHeader);

        QByteArray packet;
        packet.append((char*)&header, sizeof(header));
        return packet;
    }

    // 构造带 body 的包
    static QByteArray build(quint16 cmd, const void* body, int bodyLen)
    {
        PacketHeader header;
        header.cmd = cmd;
        header.length = sizeof(PacketHeader) + bodyLen;

        QByteArray packet;
        packet.append((char*)&header, sizeof(header));
        packet.append((char*)body, bodyLen);
        return packet;
    }

    // 模板版本
    template<typename T>
    static QByteArray build(quint16 cmd, const T& body)
    {
        return build(cmd, &body, sizeof(T));
    }
};

#endif
