#include <QCoreApplication>
#include "core.h"
#include "mysql/CMySql.h"
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QMutex>
#include <QFileInfo>
#include "mysql/mysqlconnpool.h"
static QMutex logMutex;

void messageHandler(QtMsgType type,
                    const QMessageLogContext &context,
                    const QString &msg)
{
    QMutexLocker locker(&logMutex);

    // 日志文件路径
    QString logFilePath =
        QCoreApplication::applicationDirPath()
        + "/logs/server/server.log";
    //qDebug() << logFilePath;
    // 创建目录
    QFileInfo fileInfo(logFilePath);
    QDir().mkpath(fileInfo.absolutePath());

    QFile file(logFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    QTextStream out(&file);

    // 日志等级字符串
    QString level;
    switch (type) {
    case QtDebugMsg:    level = "DEBUG"; break;
    case QtInfoMsg:     level = "INFO";  break;
    case QtWarningMsg:  level = "WARN";  break;
    case QtCriticalMsg: level = "ERROR"; break;
    case QtFatalMsg:    level = "FATAL"; break;
    }

    // 格式化日志内容
    QString text =
        QString("[%1] [%2] [%3:%4] %5()\n    %6")
            .arg(QDateTime::currentDateTime()
                     .toString("yyyy-MM-dd hh:mm:ss.zzz"))
            .arg(level)
            .arg(context.file ? context.file : "unknown")
            .arg(context.line)
            .arg(context.function ? context.function : "unknown")
            .arg(msg);

    // 1️⃣ 写文件
    out << text << "\n";
    out.flush();

    // 2️⃣ 控制台输出（不带颜色）
    fprintf(stderr, "%s\n", text.toLocal8Bit().constData());

    // 致命日志直接终止程序
    if (type == QtFatalMsg)
        abort();
}

int main(int argc, char *argv[]) {
    QCoreApplication a(argc, argv);
#ifdef DEBUG
    qInstallMessageHandler(messageHandler);
#endif
    MySqlConnPool::Instance().init(
        "127.0.0.1",           // ip
        "root",                // 用户名
        "zhangwenjie172",      // 密码
        "disk",                // 数据库名
        10                     // 连接池大小
        );
    ICore *p = core::getKernel();
    if(p->open()){
        qDebug()<<"Server is running";
    }else{
        qDebug()<<"Server failed";
    }
    return a.exec();
}
