#ifndef INET_H
#define INET_H
#include "tou.h"
#include <QObject>
class INet : public QObject{
    Q_OBJECT
public:
    explicit INet(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~INet(){}
public:
    virtual bool initNetWork(const QString& szip = SERVER_IP_LOCATION,quint16 sport = TCP_PORT_IMPORTANT_DATA)=0;
    virtual void unInitNetWork()=0;
};

#endif // INET_H
