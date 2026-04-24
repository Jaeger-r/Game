#include "serverupdatemanager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSaveFile>
#include <QTimer>

#include <cerrno>
#include <cstring>
#include <vector>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {

bool applyExecutableFlag(const QString& filePath, bool executable, QString* errorMessage)
{
    if (!executable) {
        return true;
    }

    QFile file(filePath);
    QFileDevice::Permissions permissions = file.permissions();
    permissions |= QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther;
    if (!file.setPermissions(permissions)) {
        if (errorMessage) {
            *errorMessage =
                QStringLiteral("Failed to mark %1 as executable.").arg(filePath);
        }
        return false;
    }
    return true;
}

QString normalizeAbsolutePath(const QString& path)
{
    if (path.trimmed().isEmpty()) {
        return {};
    }
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

} // namespace

ServerUpdateManager::ServerUpdateManager(const ServerRuntimeConfig& runtimeConfig,
                                         const QStringList& launchArguments)
    : m_runtimeConfig(runtimeConfig)
    , m_launchArguments(launchArguments.isEmpty() ? QCoreApplication::arguments()
                                                  : launchArguments)
{
    if (!runtimeConfig.dataDir.trimmed().isEmpty()) {
        m_dataDir = normalizeAbsolutePath(runtimeConfig.dataDir);
    }
    if (m_dataDir.isEmpty()) {
        m_dataDir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
    }

    m_configPath = QDir(m_dataDir).filePath(QStringLiteral("update_config.json"));
    m_versionPath = QDir(m_dataDir).filePath(QStringLiteral("version.json"));
    m_installRoot = QFileInfo(m_dataDir).dir().absolutePath();
}

ServerUpdateManager::StartupAction
ServerUpdateManager::maybeApplyUpdateOnLaunch(QString* errorMessage)
{
    QString localError;
    if (!loadLocalState(&localError)) {
        if (!localError.trimmed().isEmpty()) {
            qInfo().noquote() << QStringLiteral("Server update disabled: %1").arg(localError);
        }
        return StartupAction::ContinueStartup;
    }

    if (!m_config.enabled || !m_config.autoCheckOnLaunch) {
        return StartupAction::ContinueStartup;
    }

    JaegerShared::UpdateManifest manifest;
    ManifestSource source;
    if (!fetchManifest(&source, &manifest, &localError)) {
        qWarning().noquote()
            << QStringLiteral("Server update manifest check failed: %1").arg(localError);
        return StartupAction::ContinueStartup;
    }

    if (!m_config.appId.trimmed().isEmpty()
        && manifest.app.appId.compare(m_config.appId, Qt::CaseInsensitive) != 0)
    {
        qWarning().noquote()
            << QStringLiteral("Server update skipped manifest because appId mismatched: %1 != %2")
                  .arg(manifest.app.appId, m_config.appId);
        return StartupAction::ContinueStartup;
    }

    if (JaegerShared::compareVersions(manifest.app.version, localVersionString()) <= 0) {
        return StartupAction::ContinueStartup;
    }

    QString skipReason;
    if (!shouldAutoApply(&skipReason)) {
        qInfo().noquote() << skipReason;
        return StartupAction::ContinueStartup;
    }

    QString installRequestPath;
    if (!stageUpdate(source, manifest, &installRequestPath, &localError)) {
        qWarning().noquote()
            << QStringLiteral("Server update staging failed, continuing with current version: %1")
                  .arg(localError);
        return StartupAction::ContinueStartup;
    }

    switch (runUpdaterSynchronously(installRequestPath, &localError)) {
    case UpdaterRunResult::StartFailed:
        qWarning().noquote()
            << QStringLiteral("Server updater could not start, continuing with current version: %1")
                  .arg(localError);
        return StartupAction::ContinueStartup;
    case UpdaterRunResult::ApplyFailed:
        if (errorMessage) {
            *errorMessage = localError;
        }
        return StartupAction::AbortStartup;
    case UpdaterRunResult::Succeeded:
        break;
    }

    return relaunchCurrentProcess(errorMessage);
}

bool ServerUpdateManager::loadLocalState(QString* errorMessage)
{
    if (m_dataDir.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not resolve server data directory.");
        }
        return false;
    }

    if (!QFile::exists(m_configPath)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("No update_config.json found in %1.").arg(m_dataDir);
        }
        return false;
    }

    if (!JaegerShared::loadUpdateConfigFromFile(m_configPath, &m_config, errorMessage)) {
        return false;
    }

    if (QFile::exists(m_versionPath)) {
        QString versionError;
        if (JaegerShared::loadAppVersionInfoFromFile(m_versionPath, &m_localVersion, &versionError)) {
            if (m_config.appId.trimmed().isEmpty()) {
                m_config.appId = m_localVersion.appId;
            }
            if (m_config.currentVersion.trimmed().isEmpty()) {
                m_config.currentVersion = m_localVersion.version;
            }
            if (m_config.launcherPath.trimmed().isEmpty()) {
                m_config.launcherPath = m_localVersion.launcherPath;
            }
        } else {
            qWarning().noquote()
                << QStringLiteral("Server update could not parse version.json: %1").arg(versionError);
        }
    }

    if (m_config.appId.trimmed().isEmpty()) {
        m_config.appId = QStringLiteral("jaeger-server");
    }

    if (m_config.currentVersion.trimmed().isEmpty()) {
        m_config.currentVersion = QStringLiteral("0.0.0");
    }

    return true;
}

bool ServerUpdateManager::fetchManifest(ManifestSource* source,
                                        JaegerShared::UpdateManifest* manifest,
                                        QString* errorMessage)
{
    if (!source || !manifest) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Manifest output pointer is null.");
        }
        return false;
    }

    const QString manifestUrl = m_config.manifestUrl.trimmed();
    if (manifestUrl.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("manifestUrl is empty in update_config.json.");
        }
        return false;
    }

    ManifestSource prepared;
    QByteArray jsonData;

    if (JaegerShared::isRemoteUrl(manifestUrl)) {
        prepared.remoteUrl = QUrl(manifestUrl);
        if (!fetchUrlToBytes(prepared.remoteUrl, &jsonData, errorMessage)) {
            return false;
        }
    } else {
        prepared.localPath = resolveConfigPath(manifestUrl);
        if (prepared.localPath.isEmpty() || !QFile::exists(prepared.localPath)) {
            if (errorMessage) {
                *errorMessage =
                    QStringLiteral("Manifest file does not exist: %1").arg(prepared.localPath);
            }
            return false;
        }
        prepared.localBaseDir = QFileInfo(prepared.localPath).absolutePath();
        QFile manifestFile(prepared.localPath);
        if (!manifestFile.open(QIODevice::ReadOnly)) {
            if (errorMessage) {
                *errorMessage =
                    QStringLiteral("Failed to open manifest file: %1").arg(prepared.localPath);
            }
            return false;
        }
        jsonData = manifestFile.readAll();
    }

    prepared.jsonData = jsonData;
    if (!JaegerShared::parseUpdateManifest(jsonData, manifest, errorMessage)) {
        return false;
    }

    *source = prepared;
    return true;
}

bool ServerUpdateManager::shouldAutoApply(QString* skipReason) const
{
    if (!m_config.autoDownload) {
        if (skipReason) {
            *skipReason =
                QStringLiteral("Server update found a new version, but autoDownload=false so it was not applied.");
        }
        return false;
    }

    if (m_config.promptBeforeRestart) {
        if (skipReason) {
            *skipReason =
                QStringLiteral("Server update found a new version, but promptBeforeRestart=true is unsupported for unattended startup. Set it to false to auto-apply.");
        }
        return false;
    }

    return true;
}

bool ServerUpdateManager::resolveEntrySource(const JaegerShared::UpdateFileEntry& entry,
                                             const ManifestSource& source,
                                             QUrl* remoteUrl,
                                             QString* localPath,
                                             QString* errorMessage) const
{
    if (remoteUrl) {
        *remoteUrl = QUrl();
    }
    if (localPath) {
        localPath->clear();
    }

    const QString urlValue = entry.url.trimmed();
    if (!urlValue.isEmpty()) {
        if (JaegerShared::isRemoteUrl(urlValue)) {
            if (remoteUrl) {
                *remoteUrl = QUrl(urlValue);
            }
            return true;
        }

        const QUrl parsed = QUrl::fromUserInput(urlValue);
        if (parsed.isLocalFile()) {
            if (localPath) {
                *localPath = parsed.toLocalFile();
            }
            return true;
        }

        if (!source.remoteUrl.isEmpty()) {
            if (remoteUrl) {
                *remoteUrl = source.remoteUrl.resolved(QUrl(urlValue));
            }
            return true;
        }

        if (!source.localBaseDir.isEmpty()) {
            if (localPath) {
                *localPath = QDir(source.localBaseDir).filePath(urlValue);
            }
            return true;
        }
    }

    if (!entry.sourcePath.trimmed().isEmpty()) {
        if (!source.remoteUrl.isEmpty()) {
            if (remoteUrl) {
                *remoteUrl = source.remoteUrl.resolved(QUrl(entry.sourcePath));
            }
            return true;
        }

        const QString baseDir = !source.localBaseDir.isEmpty()
                                    ? source.localBaseDir
                                    : QFileInfo(source.localPath).absolutePath();
        if (!baseDir.isEmpty()) {
            if (localPath) {
                *localPath = QDir(baseDir).filePath(entry.sourcePath);
            }
            return true;
        }
    }

    if (errorMessage) {
        *errorMessage = QStringLiteral("Could not resolve update source for %1.")
                            .arg(entry.targetPath);
    }
    return false;
}

bool ServerUpdateManager::fetchUrlToBytes(const QUrl& url,
                                          QByteArray* data,
                                          QString* errorMessage)
{
    if (!data) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Download output pointer is null.");
        }
        return false;
    }

    QNetworkAccessManager network;
    QNetworkRequest request(url);
    request.setTransferTimeout(m_config.requestTimeoutMs);
    QNetworkReply* reply = network.get(request);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeoutTimer.start(m_config.requestTimeoutMs);

    loop.exec();

    if (!timeoutTimer.isActive()) {
        reply->abort();
        reply->deleteLater();
        if (errorMessage) {
            *errorMessage = QStringLiteral("Timed out fetching %1.").arg(url.toString());
        }
        return false;
    }

    timeoutTimer.stop();
    if (reply->error() != QNetworkReply::NoError) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to fetch %1: %2")
                                .arg(url.toString(), reply->errorString());
        }
        reply->deleteLater();
        return false;
    }

    *data = reply->readAll();
    reply->deleteLater();
    return true;
}

bool ServerUpdateManager::stageEntry(const JaegerShared::UpdateFileEntry& entry,
                                     const ManifestSource& source,
                                     const QString& payloadRoot,
                                     JaegerShared::UpdateFileEntry* stagedEntry,
                                     QString* errorMessage)
{
    QUrl remoteUrl;
    QString localPath;
    if (!resolveEntrySource(entry, source, &remoteUrl, &localPath, errorMessage)) {
        return false;
    }

    const QString stagedTargetPath = QDir(payloadRoot).filePath(entry.targetPath);
    if (remoteUrl.isValid()) {
        QByteArray data;
        if (!fetchUrlToBytes(remoteUrl, &data, errorMessage)) {
            return false;
        }
        if (!writeBytesToPath(data, stagedTargetPath, entry.executable, errorMessage)) {
            return false;
        }
    } else {
        if (localPath.isEmpty() || !QFile::exists(localPath)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Update source file does not exist: %1").arg(localPath);
            }
            return false;
        }
        if (!copyLocalFileToPath(localPath, stagedTargetPath, entry.executable, errorMessage)) {
            return false;
        }
    }

    const QFileInfo stagedInfo(stagedTargetPath);
    if (entry.size > 0 && stagedInfo.size() != entry.size) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Staged file size mismatch for %1.").arg(entry.targetPath);
        }
        return false;
    }

    if (!entry.sha256.trimmed().isEmpty()) {
        QString hashError;
        const QString stagedHash =
            JaegerShared::sha256HexForFile(stagedTargetPath, &hashError).toLower();
        if (stagedHash != entry.sha256.trimmed().toLower()) {
            if (errorMessage) {
                *errorMessage = hashError.isEmpty()
                                    ? QStringLiteral("SHA256 mismatch for %1.").arg(entry.targetPath)
                                    : hashError;
            }
            return false;
        }
    }

    if (!stagedEntry) {
        return true;
    }

    *stagedEntry = entry;
    stagedEntry->sourcePath =
        JaegerShared::normalizedRelativeUpdatePath(QDir(QStringLiteral("payload"))
                                                       .filePath(entry.targetPath));
    stagedEntry->url.clear();
    return true;
}

bool ServerUpdateManager::stageUpdate(const ManifestSource& source,
                                      const JaegerShared::UpdateManifest& manifest,
                                      QString* installRequestPath,
                                      QString* errorMessage)
{
    if (!installRequestPath) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Install request output pointer is null.");
        }
        return false;
    }

    const QString appId = !manifest.app.appId.trimmed().isEmpty()
                              ? manifest.app.appId
                              : m_config.appId;
    const QString stagingDir =
        JaegerShared::defaultUpdateStagingDir(appId, manifest.app.version);

    QDir stagingRoot(stagingDir);
    if (stagingRoot.exists() && !stagingRoot.removeRecursively()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to clear staging dir: %1").arg(stagingDir);
        }
        return false;
    }
    if (!QDir().mkpath(stagingDir)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create staging dir: %1").arg(stagingDir);
        }
        return false;
    }

    const QString payloadRoot = QDir(stagingDir).filePath(QStringLiteral("payload"));
    if (!QDir().mkpath(payloadRoot)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create payload dir: %1").arg(payloadRoot);
        }
        return false;
    }

    JaegerShared::UpdateManifest stagedManifest = manifest;
    for (int index = 0; index < stagedManifest.files.size(); ++index) {
        JaegerShared::UpdateFileEntry stagedFileEntry;
        if (!stageEntry(stagedManifest.files[index],
                        source,
                        payloadRoot,
                        &stagedFileEntry,
                        errorMessage))
        {
            return false;
        }
        stagedManifest.files[index] = stagedFileEntry;
    }

    const QString stagedManifestPath = QDir(stagingDir).filePath(QStringLiteral("manifest.json"));
    if (!JaegerShared::saveUpdateManifestToFile(stagedManifestPath, stagedManifest, errorMessage)) {
        return false;
    }

    JaegerShared::UpdateInstallRequest request;
    request.displayName = normalizedDisplayName(manifest, m_localVersion);
    request.targetRoot = installRootDirectory();
    request.stagingDir = stagingDir;
    request.manifestFilePath = stagedManifestPath;
    request.relaunchPath = resolveLauncherPath();
    request.versionFilePath = resolveVersionFilePath();
    request.relaunchArguments = m_launchArguments.mid(1);
    request.waitForPid = 0;
    request.relaunchAfterInstall = false;
    request.cleanupAfterInstall = true;

    const QString requestPath =
        QDir(stagingDir).filePath(QStringLiteral("install_request.json"));
    if (!JaegerShared::saveUpdateInstallRequestToFile(requestPath, request, errorMessage)) {
        return false;
    }

    *installRequestPath = requestPath;
    return true;
}

ServerUpdateManager::UpdaterRunResult
ServerUpdateManager::runUpdaterSynchronously(const QString& installRequestPath,
                                            QString* errorMessage) const
{
    const QString updaterPath = resolveUpdaterPath();
    if (updaterPath.isEmpty() || !QFile::exists(updaterPath)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Updater executable was not found: %1").arg(updaterPath);
        }
        return UpdaterRunResult::StartFailed;
    }

    QProcess updaterProcess;
    updaterProcess.setProgram(updaterPath);
    updaterProcess.setArguments({
        QStringLiteral("--headless"),
        QStringLiteral("--install-request"),
        installRequestPath
    });
    updaterProcess.setWorkingDirectory(QFileInfo(updaterPath).absolutePath());
    updaterProcess.setProcessChannelMode(QProcess::MergedChannels);
    updaterProcess.start();

    if (!updaterProcess.waitForStarted(10000)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to start updater: %1").arg(updaterPath);
        }
        return UpdaterRunResult::StartFailed;
    }

    if (!updaterProcess.waitForFinished(-1)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Timed out waiting for updater to finish.");
        }
        return UpdaterRunResult::ApplyFailed;
    }

    const QString updaterOutput =
        QString::fromLocal8Bit(updaterProcess.readAllStandardOutput()).trimmed();
    if (!updaterOutput.isEmpty()) {
        qInfo().noquote() << updaterOutput;
    }

    if (updaterProcess.exitStatus() != QProcess::NormalExit || updaterProcess.exitCode() != 0) {
        if (errorMessage) {
            *errorMessage = updaterOutput.isEmpty()
                                ? QStringLiteral("Updater failed with exit code %1.")
                                      .arg(updaterProcess.exitCode())
                                : QStringLiteral("Updater failed: %1").arg(updaterOutput);
        }
        return UpdaterRunResult::ApplyFailed;
    }

    return UpdaterRunResult::Succeeded;
}

ServerUpdateManager::StartupAction
ServerUpdateManager::relaunchCurrentProcess(QString* errorMessage) const
{
    QStringList arguments = m_launchArguments;
    if (arguments.isEmpty()) {
        arguments = QCoreApplication::arguments();
    }

    QString programPath = resolveLauncherPath();
    if (programPath.trimmed().isEmpty()) {
        programPath = QCoreApplication::applicationFilePath();
    }
    if (programPath.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not resolve relaunch target for GameServer.");
        }
        return StartupAction::AbortStartup;
    }

    if (arguments.isEmpty()) {
        arguments.push_back(programPath);
    } else {
        arguments[0] = programPath;
    }

#ifdef Q_OS_UNIX
    std::vector<QByteArray> encodedArguments;
    encodedArguments.reserve(arguments.size());
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);

    for (const QString& argument : arguments) {
        encodedArguments.push_back(QFile::encodeName(argument));
    }
    for (QByteArray& encoded : encodedArguments) {
        argv.push_back(encoded.data());
    }
    argv.push_back(nullptr);

    ::execv(encodedArguments.front().constData(), argv.data());
#endif

    if (QProcess::startDetached(programPath,
                                arguments.mid(1),
                                QFileInfo(programPath).absolutePath()))
    {
        return StartupAction::Restarting;
    }

    if (errorMessage) {
#ifdef Q_OS_UNIX
        *errorMessage = QStringLiteral("Failed to relaunch server via exec/startDetached: %1")
                            .arg(QString::fromLocal8Bit(std::strerror(errno)));
#else
        *errorMessage = QStringLiteral("Failed to relaunch server: %1").arg(programPath);
#endif
    }
    return StartupAction::AbortStartup;
}

QString ServerUpdateManager::dataDirectory() const
{
    return m_dataDir;
}

QString ServerUpdateManager::installRootDirectory() const
{
    return m_installRoot;
}

QString ServerUpdateManager::resolveConfigPath(const QString& value) const
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    const QFileInfo fileInfo(trimmed);
    if (fileInfo.isAbsolute()) {
        return fileInfo.absoluteFilePath();
    }

    return QDir(dataDirectory()).filePath(trimmed);
}

QString ServerUpdateManager::resolveRelativeToInstallRoot(const QString& value) const
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    const QFileInfo fileInfo(trimmed);
    if (fileInfo.isAbsolute()) {
        return fileInfo.absoluteFilePath();
    }

    return QDir(installRootDirectory()).filePath(trimmed);
}

QString ServerUpdateManager::resolveLauncherPath() const
{
    if (!m_config.launcherPath.trimmed().isEmpty()) {
        return resolveRelativeToInstallRoot(m_config.launcherPath);
    }
    if (!m_localVersion.launcherPath.trimmed().isEmpty()) {
        return resolveRelativeToInstallRoot(m_localVersion.launcherPath);
    }
    return QCoreApplication::applicationFilePath();
}

QString ServerUpdateManager::resolveUpdaterPath() const
{
    if (!m_config.updaterPath.trimmed().isEmpty()) {
        return resolveRelativeToInstallRoot(m_config.updaterPath);
    }

    const QStringList candidates = {
        QStringLiteral("JaegerUpdater.app/Contents/MacOS/JaegerUpdater"),
        QStringLiteral("JaegerUpdater")
    };
    for (const QString& candidate : candidates) {
        const QString resolved = resolveRelativeToInstallRoot(candidate);
        if (QFile::exists(resolved)) {
            return resolved;
        }
    }
    return resolveRelativeToInstallRoot(candidates.last());
}

QString ServerUpdateManager::resolveVersionFilePath() const
{
    if (!m_config.versionFilePath.trimmed().isEmpty()) {
        return resolveRelativeToInstallRoot(m_config.versionFilePath);
    }
    return m_versionPath;
}

QString ServerUpdateManager::localVersionString() const
{
    if (!m_localVersion.version.trimmed().isEmpty()) {
        return m_localVersion.version;
    }
    return m_config.currentVersion.trimmed().isEmpty()
               ? QStringLiteral("0.0.0")
               : m_config.currentVersion.trimmed();
}

QString ServerUpdateManager::normalizedDisplayName(const JaegerShared::UpdateManifest& manifest,
                                                   const JaegerShared::AppVersionInfo& localVersion)
{
    if (!manifest.app.displayName.trimmed().isEmpty()) {
        return manifest.app.displayName.trimmed();
    }
    if (!localVersion.displayName.trimmed().isEmpty()) {
        return localVersion.displayName.trimmed();
    }
    if (!manifest.app.appId.trimmed().isEmpty()) {
        return manifest.app.appId.trimmed();
    }
    return QStringLiteral("Jaeger GameServer");
}

bool ServerUpdateManager::copyLocalFileToPath(const QString& sourcePath,
                                              const QString& targetPath,
                                              bool executable,
                                              QString* errorMessage)
{
    QFile sourceFile(sourcePath);
    if (!sourceFile.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to open source file: %1").arg(sourcePath);
        }
        return false;
    }

    QFileInfo targetInfo(targetPath);
    if (!QDir().mkpath(targetInfo.absolutePath())) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create directory: %1")
                                .arg(targetInfo.absolutePath());
        }
        return false;
    }

    QSaveFile targetFile(targetPath);
    if (!targetFile.open(QIODevice::WriteOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to open target file: %1").arg(targetPath);
        }
        return false;
    }

    while (!sourceFile.atEnd()) {
        const QByteArray chunk = sourceFile.read(256 * 1024);
        if (chunk.isEmpty() && sourceFile.error() != QFile::NoError) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed while reading %1.").arg(sourcePath);
            }
            return false;
        }
        if (targetFile.write(chunk) != chunk.size()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed while writing %1.").arg(targetPath);
            }
            return false;
        }
    }

    if (!targetFile.commit()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to finalize %1.").arg(targetPath);
        }
        return false;
    }

    return applyExecutableFlag(targetPath, executable, errorMessage);
}

bool ServerUpdateManager::writeBytesToPath(const QByteArray& data,
                                           const QString& targetPath,
                                           bool executable,
                                           QString* errorMessage)
{
    QFileInfo targetInfo(targetPath);
    if (!QDir().mkpath(targetInfo.absolutePath())) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create directory: %1")
                                .arg(targetInfo.absolutePath());
        }
        return false;
    }

    QSaveFile targetFile(targetPath);
    if (!targetFile.open(QIODevice::WriteOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to open target file: %1").arg(targetPath);
        }
        return false;
    }

    if (targetFile.write(data) != data.size()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to write target file: %1").arg(targetPath);
        }
        return false;
    }

    if (!targetFile.commit()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to finalize %1.").arg(targetPath);
        }
        return false;
    }

    return applyExecutableFlag(targetPath, executable, errorMessage);
}
