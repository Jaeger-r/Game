#ifndef SERVERCONFIG_H
#define SERVERCONFIG_H

#include <QString>
#include <QtGlobal>

struct ServerRuntimeConfig
{
    bool headless = false;
    QString configPath;

    QString listenHost = QStringLiteral("0.0.0.0");
    quint16 tcpPort = 1234;
    quint16 kcpPort = 1235;
    QString dataDir;
    QString logDir;

    QString postgresHost = QStringLiteral("127.0.0.1");
    quint16 postgresPort = 5432;
    QString postgresUser = QStringLiteral("jaeger_server");
    QString postgresPassword;
    QString postgresDatabase = QStringLiteral("disk");
    int postgresPoolSize = 10;
};

ServerRuntimeConfig loadServerRuntimeConfig(int argc,
                                           char* argv[],
                                           QString* warningMessage = nullptr);

#endif // SERVERCONFIG_H
