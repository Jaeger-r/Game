#ifndef SERVERUPDATEMANAGER_H
#define SERVERUPDATEMANAGER_H

#include "../Shared/sharedupdate.h"
#include "serverconfig.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>

class ServerUpdateManager
{
public:
    enum class StartupAction
    {
        ContinueStartup,
        Restarting,
        AbortStartup
    };

    explicit ServerUpdateManager(const ServerRuntimeConfig& runtimeConfig,
                                 const QStringList& launchArguments = {});

    StartupAction maybeApplyUpdateOnLaunch(QString* errorMessage = nullptr);

private:
    enum class UpdaterRunResult
    {
        StartFailed,
        ApplyFailed,
        Succeeded
    };

    struct ManifestSource
    {
        QByteArray jsonData;
        QString localPath;
        QString localBaseDir;
        QUrl remoteUrl;
    };

    bool loadLocalState(QString* errorMessage);
    bool fetchManifest(ManifestSource* source,
                       JaegerShared::UpdateManifest* manifest,
                       QString* errorMessage);
    bool shouldAutoApply(QString* skipReason) const;
    bool resolveEntrySource(const JaegerShared::UpdateFileEntry& entry,
                            const ManifestSource& source,
                            QUrl* remoteUrl,
                            QString* localPath,
                            QString* errorMessage) const;
    bool fetchUrlToBytes(const QUrl& url, QByteArray* data, QString* errorMessage);
    bool stageEntry(const JaegerShared::UpdateFileEntry& entry,
                    const ManifestSource& source,
                    const QString& payloadRoot,
                    JaegerShared::UpdateFileEntry* stagedEntry,
                    QString* errorMessage);
    bool stageUpdate(const ManifestSource& source,
                     const JaegerShared::UpdateManifest& manifest,
                     QString* installRequestPath,
                     QString* errorMessage);
    UpdaterRunResult runUpdaterSynchronously(const QString& installRequestPath,
                                             QString* errorMessage) const;
    StartupAction relaunchCurrentProcess(QString* errorMessage) const;

    QString dataDirectory() const;
    QString installRootDirectory() const;
    QString resolveConfigPath(const QString& value) const;
    QString resolveRelativeToInstallRoot(const QString& value) const;
    QString resolveLauncherPath() const;
    QString resolveUpdaterPath() const;
    QString resolveVersionFilePath() const;
    QString localVersionString() const;

    static QString normalizedDisplayName(const JaegerShared::UpdateManifest& manifest,
                                         const JaegerShared::AppVersionInfo& localVersion);
    static bool copyLocalFileToPath(const QString& sourcePath,
                                    const QString& targetPath,
                                    bool executable,
                                    QString* errorMessage);
    static bool writeBytesToPath(const QByteArray& data,
                                 const QString& targetPath,
                                 bool executable,
                                 QString* errorMessage);

    ServerRuntimeConfig m_runtimeConfig;
    JaegerShared::UpdateConfig m_config;
    JaegerShared::AppVersionInfo m_localVersion;
    QStringList m_launchArguments;
    QString m_dataDir;
    QString m_configPath;
    QString m_versionPath;
    QString m_installRoot;
};

#endif // SERVERUPDATEMANAGER_H
