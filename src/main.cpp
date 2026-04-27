#ifdef JAEGER_HEADLESS_BUILD
#include <QCoreApplication>
#else
#include <QApplication>
#include <QCoreApplication>
#endif
#include "core.h"
#include "mysql/CMySql.h"
#include "serverconfig.h"
#include "serverupdatemanager.h"
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QMutex>
#include <QFileInfo>
#include <memory>
#include "mysql/mysqlconnpool.h"
#include "servermonitorbridge.h"
#ifndef JAEGER_HEADLESS_BUILD
#include "servermonitorwindow.h"
#endif
static QMutex logMutex;
static QString g_logFilePath;

/**
 * @brief 处理messageHandler相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void messageHandler(QtMsgType type,
                    const QMessageLogContext &context,
                    const QString &msg)
{
    QMutexLocker locker(&logMutex);

    // 日志文件路径
    QString logFilePath = g_logFilePath.trimmed();
    if (logFilePath.isEmpty()) {
        logFilePath =
            QCoreApplication::applicationDirPath()
            + "/logs/server/server.log";
    }
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
    ServerMonitorBridge::instance()->appendLogLine(text);

    // 致命日志直接终止程序
    if (type == QtFatalMsg)
        abort();
}

/**
 * @brief 程序主入口，负责初始化并启动游戏
 * @author Jaeger
 * @date 2025.3.28
 */
int main(int argc, char *argv[]) {
    QString configWarning;
    ServerRuntimeConfig runtimeConfig =
        loadServerRuntimeConfig(argc, argv, &configWarning);

    std::unique_ptr<QCoreApplication> app;
#ifdef JAEGER_HEADLESS_BUILD
    runtimeConfig.headless = true;
    app = std::make_unique<QCoreApplication>(argc, argv);
#else
    if (runtimeConfig.headless) {
        app = std::make_unique<QCoreApplication>(argc, argv);
    } else {
        app = std::make_unique<QApplication>(argc, argv);
    }
#endif

    QString resolvedLogDir = runtimeConfig.logDir.trimmed();
    if (resolvedLogDir.isEmpty()) {
        resolvedLogDir = QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("logs/server"));
    }
    QDir().mkpath(resolvedLogDir);
    g_logFilePath = QDir(resolvedLogDir).filePath(QStringLiteral("server.log"));

    qInstallMessageHandler(messageHandler);

    if (!configWarning.trimmed().isEmpty()) {
        qWarning().noquote() << configWarning;
    }

    ServerUpdateManager updateManager(runtimeConfig, app->arguments());
    QString updateError;
    switch (updateManager.maybeApplyUpdateOnLaunch(&updateError)) {
    case ServerUpdateManager::StartupAction::ContinueStartup:
        break;
    case ServerUpdateManager::StartupAction::Restarting:
        return 0;
    case ServerUpdateManager::StartupAction::AbortStartup:
        qCritical().noquote()
            << QStringLiteral("Server update failed and startup was aborted: %1").arg(updateError);
        return 1;
    }

    
#ifndef JAEGER_HEADLESS_BUILD
    std::unique_ptr<ServerMonitorWindow> monitor;
    if (!runtimeConfig.headless) {
        monitor = std::make_unique<ServerMonitorWindow>();
        monitor->show();
    } else {
        qInfo() << "Running in headless mode with config"
                << runtimeConfig.configPath;
    }
#else
    qInfo() << "Running in headless mode with config"
            << runtimeConfig.configPath;
#endif

    try {
        MySqlConnPool::Instance().init(
            runtimeConfig.postgresHost.toStdString(),
            runtimeConfig.postgresUser.toStdString(),
            runtimeConfig.postgresPassword.toStdString(),
            runtimeConfig.postgresDatabase.toStdString(),
            runtimeConfig.postgresPort,
            runtimeConfig.postgresPoolSize
            );
    } catch (const std::exception& ex) {
        qCritical().noquote() << QStringLiteral("Failed to initialize PostgreSQL pool: %1")
                                     .arg(QString::fromLocal8Bit(ex.what()));
        return 1;
    } catch (...) {
        qCritical() << "Failed to initialize PostgreSQL pool due to unknown exception.";
        return 1;
    }

    core* kernel = static_cast<core*>(core::getKernel());
    kernel->applyRuntimeConfig(runtimeConfig);
    ICore *p = kernel;
    if(p->open()){
        qDebug()<<"Server is running";
    }else{
        qDebug()<<"Server failed";
        return 1;
    }
    return app->exec();
}
