#include "serverconfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcessEnvironment>
#include <QTextStream>

namespace {

void appendWarning(QString* warningMessage, const QString& text)
{
    if (!warningMessage || text.trimmed().isEmpty()) {
        return;
    }

    if (warningMessage->trimmed().isEmpty()) {
        *warningMessage = text.trimmed();
        return;
    }

    *warningMessage += QLatin1Char('\n') + text.trimmed();
}

QString valueOrDefault(const QJsonObject& object,
                       const QString& key,
                       const QString& fallback)
{
    const QJsonValue value = object.value(key);
    return value.isString() ? value.toString().trimmed() : fallback;
}

int intOrDefault(const QJsonObject& object, const QString& key, int fallback)
{
    const QJsonValue value = object.value(key);
    return value.isDouble() ? value.toInt(fallback) : fallback;
}

QString normalizedAbsolutePath(const QString& path)
{
    if (path.trimmed().isEmpty()) {
        return {};
    }
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString resolvePathFromBase(const QString& value, const QString& baseDir)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    const QFileInfo info(trimmed);
    if (info.isAbsolute()) {
        return QDir::cleanPath(info.absoluteFilePath());
    }

    const QString resolvedBase = baseDir.trimmed().isEmpty()
                                     ? QDir::currentPath()
                                     : QFileInfo(baseDir).absoluteFilePath();
    return QDir(resolvedBase).absoluteFilePath(trimmed);
}

void appendSearchRoot(QStringList* roots, const QString& path)
{
    if (!roots) {
        return;
    }

    const QString normalized = normalizedAbsolutePath(path);
    if (normalized.isEmpty()) {
        return;
    }

    const QFileInfo info(normalized);
    if (!info.exists() || !info.isDir()) {
        return;
    }

    if (!roots->contains(normalized)) {
        roots->push_back(normalized);
    }
}

QString executableDirectory(int argc, char* argv[])
{
    if (argc <= 0 || !argv || !argv[0]) {
        return QDir::currentPath();
    }
    return QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
}

QStringList buildSearchRoots(int argc, char* argv[])
{
    QStringList roots;
    const QString execDir = executableDirectory(argc, argv);
    const QString currentDir = QDir::currentPath();

    appendSearchRoot(&roots, execDir);
    appendSearchRoot(&roots, currentDir);

    const QStringList seeds = {execDir, currentDir};
    for (const QString& seed : seeds) {
        QDir dir(seed);
        for (int depth = 0; depth < 8; ++depth) {
            const QString absolute = dir.absolutePath();
            appendSearchRoot(&roots, absolute);
            appendSearchRoot(&roots, QDir(absolute).filePath(QStringLiteral("GameServer")));
            if (!dir.cdUp()) {
                break;
            }
        }
    }

    return roots;
}

QString findFirstExistingFile(const QStringList& roots, const QStringList& relativePaths)
{
    for (const QString& root : roots) {
        for (const QString& relative : relativePaths) {
            const QFileInfo info(QDir(root).filePath(relative));
            if (info.exists() && info.isFile()) {
                return info.absoluteFilePath();
            }
        }
    }
    return {};
}

QString findFirstExistingDirectory(const QStringList& roots,
                                   const QStringList& relativePaths,
                                   const QStringList& markerFiles)
{
    for (const QString& root : roots) {
        for (const QString& relative : relativePaths) {
            const QFileInfo info(QDir(root).filePath(relative));
            if (!info.exists() || !info.isDir()) {
                continue;
            }

            bool matchedMarker = markerFiles.isEmpty();
            for (const QString& marker : markerFiles) {
                if (QFileInfo::exists(QDir(info.absoluteFilePath()).filePath(marker))) {
                    matchedMarker = true;
                    break;
                }
            }

            if (matchedMarker) {
                return info.absoluteFilePath();
            }
        }
    }

    return {};
}

QString stripOptionalQuotes(const QString& value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.size() >= 2) {
        const QChar first = trimmed.front();
        const QChar last = trimmed.back();
        if ((first == QLatin1Char('"') && last == QLatin1Char('"'))
            || (first == QLatin1Char('\'') && last == QLatin1Char('\'')))
        {
            return trimmed.mid(1, trimmed.size() - 2);
        }
    }
    return trimmed;
}

bool loadEnvFileValues(const QString& filePath,
                       QHash<QString, QString>* values,
                       QString* errorMessage)
{
    if (!values) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Env map output pointer is null.");
        }
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to open env file: %1").arg(filePath);
        }
        return false;
    }

    QTextStream stream(&file);
    int lineNumber = 0;
    while (!stream.atEnd()) {
        ++lineNumber;
        QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        if (line.startsWith(QStringLiteral("export "))) {
            line = line.mid(QStringLiteral("export ").size()).trimmed();
        }

        const int equalIndex = line.indexOf(QLatin1Char('='));
        if (equalIndex <= 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Invalid env line %1 in %2.")
                                    .arg(lineNumber)
                                    .arg(filePath);
            }
            return false;
        }

        const QString key = line.left(equalIndex).trimmed();
        const QString value = stripOptionalQuotes(line.mid(equalIndex + 1));
        if (!key.isEmpty()) {
            values->insert(key, value);
        }
    }

    return true;
}

QString envMapValueOrDefault(const QHash<QString, QString>& values,
                             const QString& key,
                             const QString& fallback)
{
    const QString value = values.value(key).trimmed();
    return value.isEmpty() ? fallback : value;
}

int envMapIntOrDefault(const QHash<QString, QString>& values, const QString& key, int fallback)
{
    bool ok = false;
    const int value = values.value(key).trimmed().toInt(&ok);
    return ok ? value : fallback;
}

void applyRuntimeOverridesFromMap(ServerRuntimeConfig& config,
                                  const QHash<QString, QString>& values,
                                  const QString& baseDir)
{
    config.listenHost =
        envMapValueOrDefault(values, QStringLiteral("JAEGER_SERVER_LISTEN_HOST"), config.listenHost);
    config.tcpPort = static_cast<quint16>(
        qBound(1, envMapIntOrDefault(values, QStringLiteral("JAEGER_SERVER_TCP_PORT"), config.tcpPort), 65535));
    config.kcpPort = static_cast<quint16>(
        qBound(1, envMapIntOrDefault(values, QStringLiteral("JAEGER_SERVER_KCP_PORT"), config.kcpPort), 65535));

    const QString dataDir = envMapValueOrDefault(values, QStringLiteral("JAEGER_SERVER_DATA_DIR"), QString());
    if (!dataDir.trimmed().isEmpty()) {
        config.dataDir = resolvePathFromBase(dataDir, baseDir);
    }

    const QString logDir = envMapValueOrDefault(values, QStringLiteral("JAEGER_SERVER_LOG_DIR"), QString());
    if (!logDir.trimmed().isEmpty()) {
        config.logDir = resolvePathFromBase(logDir, baseDir);
    }

    config.mysqlHost =
        envMapValueOrDefault(values, QStringLiteral("JAEGER_SERVER_MYSQL_HOST"), config.mysqlHost);
    config.mysqlPort = static_cast<quint16>(
        qBound(1, envMapIntOrDefault(values, QStringLiteral("JAEGER_SERVER_MYSQL_PORT"), config.mysqlPort), 65535));
    config.mysqlUser =
        envMapValueOrDefault(values, QStringLiteral("JAEGER_SERVER_MYSQL_USER"), config.mysqlUser);
    config.mysqlPassword =
        envMapValueOrDefault(values, QStringLiteral("JAEGER_SERVER_MYSQL_PASSWORD"), config.mysqlPassword);
    config.mysqlDatabase =
        envMapValueOrDefault(values, QStringLiteral("JAEGER_SERVER_MYSQL_DATABASE"), config.mysqlDatabase);
    config.mysqlPoolSize = qMax(
        1, envMapIntOrDefault(values, QStringLiteral("JAEGER_SERVER_MYSQL_POOL_SIZE"), config.mysqlPoolSize));
}

QString resolveConfigPath(int argc, char* argv[])
{
    QString cliConfigPath;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]).trimmed();
        if (arg == QStringLiteral("--config") && i + 1 < argc) {
            cliConfigPath = QString::fromLocal8Bit(argv[++i]).trimmed();
        } else if (arg.startsWith(QStringLiteral("--config="))) {
            cliConfigPath = arg.mid(QStringLiteral("--config=").size()).trimmed();
        }
    }

    if (!cliConfigPath.isEmpty()) {
        return QFileInfo(cliConfigPath).absoluteFilePath();
    }

    const QString envConfigPath =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("JAEGER_SERVER_CONFIG")).trimmed();
    if (!envConfigPath.isEmpty()) {
        return QFileInfo(envConfigPath).absoluteFilePath();
    }

    const QStringList searchRoots = buildSearchRoots(argc, argv);
    const QString discoveredConfig = findFirstExistingFile(
        searchRoots,
        {QStringLiteral("server.json"), QStringLiteral("GameServer/server.json")});
    if (!discoveredConfig.isEmpty()) {
        return discoveredConfig;
    }

    return QDir(executableDirectory(argc, argv)).filePath(QStringLiteral("server.json"));
}

bool hasCliFlag(int argc, char* argv[], const QString& expected)
{
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]).trimmed() == expected) {
            return true;
        }
    }
    return false;
}

} // namespace

ServerRuntimeConfig loadServerRuntimeConfig(int argc,
                                           char* argv[],
                                           QString* warningMessage)
{
    ServerRuntimeConfig config;
    const QStringList searchRoots = buildSearchRoots(argc, argv);
    config.configPath = resolveConfigPath(argc, argv);
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString configBaseDir = QFileInfo(config.configPath).exists()
                                      ? QFileInfo(config.configPath).absolutePath()
                                      : QString();

    const bool forceHeadless = hasCliFlag(argc, argv, QStringLiteral("--headless"));
    const bool forceGui = hasCliFlag(argc, argv, QStringLiteral("--gui"));

    bool loadedConfigFile = false;
    if (QFileInfo::exists(config.configPath)) {
        QFile file(config.configPath);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QJsonParseError actualError;
            const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &actualError);
            if (actualError.error == QJsonParseError::NoError && doc.isObject()) {
                const QJsonObject root = doc.object();
                const QJsonObject server = root.value(QStringLiteral("server")).toObject();
                const QJsonObject mysql = root.value(QStringLiteral("mysql")).toObject();
                loadedConfigFile = true;

                const QString mode =
                    valueOrDefault(server, QStringLiteral("mode"), QStringLiteral("gui")).toLower();
                config.headless = (mode == QStringLiteral("headless"));
                config.listenHost = valueOrDefault(server, QStringLiteral("listenHost"), config.listenHost);
                config.tcpPort = static_cast<quint16>(qBound(
                    1, intOrDefault(server, QStringLiteral("tcpPort"), config.tcpPort), 65535));
                config.kcpPort = static_cast<quint16>(qBound(
                    1, intOrDefault(server, QStringLiteral("kcpPort"), config.kcpPort), 65535));
                config.dataDir = resolvePathFromBase(
                    valueOrDefault(server, QStringLiteral("dataDir"), config.dataDir), configBaseDir);
                config.logDir = resolvePathFromBase(
                    valueOrDefault(server, QStringLiteral("logDir"), config.logDir), configBaseDir);

                config.mysqlHost = valueOrDefault(mysql, QStringLiteral("host"), config.mysqlHost);
                config.mysqlPort = static_cast<quint16>(qBound(
                    1, intOrDefault(mysql, QStringLiteral("port"), config.mysqlPort), 65535));
                config.mysqlUser = valueOrDefault(mysql, QStringLiteral("user"), config.mysqlUser);
                config.mysqlPassword = valueOrDefault(mysql, QStringLiteral("password"), config.mysqlPassword);
                config.mysqlDatabase = valueOrDefault(mysql, QStringLiteral("database"), config.mysqlDatabase);
                config.mysqlPoolSize =
                    qMax(1, intOrDefault(mysql, QStringLiteral("poolSize"), config.mysqlPoolSize));
            } else {
                appendWarning(warningMessage,
                              QStringLiteral("Failed to parse %1: %2")
                                  .arg(config.configPath, actualError.errorString()));
            }
        } else {
            appendWarning(warningMessage,
                          QStringLiteral("Failed to open %1 for reading.")
                              .arg(config.configPath));
        }
    }

    QString envFilePath = env.value(QStringLiteral("JAEGER_SERVER_ENV")).trimmed();
    if (!envFilePath.isEmpty()) {
        envFilePath = QFileInfo(envFilePath).absoluteFilePath();
    } else {
        envFilePath = findFirstExistingFile(
            searchRoots,
            {QStringLiteral("server.env"), QStringLiteral("GameServer/server.env")});
    }

    bool loadedEnvFile = false;
    if (!envFilePath.isEmpty() && QFileInfo::exists(envFilePath)) {
        QHash<QString, QString> envFileValues;
        QString envFileError;
        if (loadEnvFileValues(envFilePath, &envFileValues, &envFileError)) {
            loadedEnvFile = true;
            applyRuntimeOverridesFromMap(
                config,
                envFileValues,
                QFileInfo(envFilePath).absolutePath());
        } else {
            appendWarning(warningMessage, envFileError);
        }
    }

    QHash<QString, QString> processEnvValues;
    const QStringList overrideKeys = {
        QStringLiteral("JAEGER_SERVER_LISTEN_HOST"),
        QStringLiteral("JAEGER_SERVER_TCP_PORT"),
        QStringLiteral("JAEGER_SERVER_KCP_PORT"),
        QStringLiteral("JAEGER_SERVER_DATA_DIR"),
        QStringLiteral("JAEGER_SERVER_LOG_DIR"),
        QStringLiteral("JAEGER_SERVER_MYSQL_HOST"),
        QStringLiteral("JAEGER_SERVER_MYSQL_PORT"),
        QStringLiteral("JAEGER_SERVER_MYSQL_USER"),
        QStringLiteral("JAEGER_SERVER_MYSQL_PASSWORD"),
        QStringLiteral("JAEGER_SERVER_MYSQL_DATABASE"),
        QStringLiteral("JAEGER_SERVER_MYSQL_POOL_SIZE")
    };
    for (const QString& key : overrideKeys) {
        if (env.contains(key)) {
            processEnvValues.insert(key, env.value(key));
        }
    }
    applyRuntimeOverridesFromMap(config, processEnvValues, QDir::currentPath());

    if (config.dataDir.trimmed().isEmpty()) {
        config.dataDir = findFirstExistingDirectory(
            searchRoots,
            {QStringLiteral("data"), QStringLiteral("GameServer/data")},
            {QStringLiteral("update_config.json"), QStringLiteral("version.json")});
    }

    if (config.logDir.trimmed().isEmpty()) {
        const QString logRoot = !config.dataDir.trimmed().isEmpty()
                                    ? QFileInfo(config.dataDir).dir().absolutePath()
                                    : executableDirectory(argc, argv);
        config.logDir = QDir(logRoot).filePath(QStringLiteral("logs"));
    }

    if (!loadedConfigFile) {
        appendWarning(warningMessage,
                      QStringLiteral("No server.json was loaded; using fallback paths and env overrides."));
    }
    if (!loadedEnvFile && config.mysqlPassword.trimmed().isEmpty()) {
        appendWarning(
            warningMessage,
            QStringLiteral("MySQL password is empty. Create GameServer/server.env or set JAEGER_SERVER_MYSQL_PASSWORD."));
    }

    if (forceHeadless) {
        config.headless = true;
    } else if (forceGui) {
        config.headless = false;
    }

    return config;
}
