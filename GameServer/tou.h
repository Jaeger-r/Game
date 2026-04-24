#ifndef TOU_H
#define TOU_H

#define HOSTIP "127.0.0.1"
#define HOSTPORT 1234

#include <QTcpSocket>
#include <QTcpServer>
#include <qthread.h>
#include <map>
#include <QByteArray>
#include <QDateTime>

enum PlayerDirection{ n,w,e,s,nw,ne,sw,se };
#define MAX_BAG_ITEM_NUM 48
// 网络端口配置
#define TCP_PORT_IMPORTANT_DATA  1234  // TCP端口：处理重要数据（登录、交易、关键操作等）
#define KCP_PORT_REALTIME_MOVE   1235  // KCP端口：处理实时移动（玩家位置、游戏状态同步等）
#define SERVER_IP_LOCATION "127.0.0.1"
#define DEBUG

#ifdef DEBUG
#define JaegerDebug() qDebug()<< "[" << __FILE__ << ":" << __LINE__ << "]" << QDateTime::currentDateTime().toString("hh:mm:ss:zzz") << __FUNCTION__ << "()"
#define JaegerInfo() qWarning()<< "[" << __FILE__ << ":" << __LINE__ << "]" << QDateTime::currentDateTime().toString("hh:mm:ss:zzz") << __FUNCTION__ << "()"
#define JaegerCritical() qCritical()<< "[" << __FILE__ << ":" << __LINE__ << "]" << QDateTime::currentDateTime().toString("hh:mm:ss:zzz") << __FUNCTION__ << "()"
#endif

#endif // TOU_H
