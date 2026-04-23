#ifndef ICORE_H
#define ICORE_H
#include <QTcpSocket>
#include <fstream>
#include <string>
#include <ctime>
#include <QObject>
class ICore : public QObject{
    Q_OBJECT
public:
    explicit ICore(QObject* parent = nullptr) : QObject(parent) {};
    virtual ~ICore(){};
public:
    virtual bool open() = 0;
    virtual void close() = 0;
    virtual void dealData(quint64 clientId, const QByteArray& data) = 0;
};

#endif // ICORE_H
