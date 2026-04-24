#ifndef SERVERMONITORTYPES_H
#define SERVERMONITORTYPES_H

#include <QDateTime>
#include <QVector>
#include <QString>

struct ServerModuleStatus
{
    bool tcpOk = false;
    bool kcpOk = false;
    bool dbOk = false;
    bool fileOk = false;
};

struct TcpMonitorStats
{
    bool listening = false;
    quint16 port = 0;
    int onlineConnections = 0;
    int logicActive = 0;
    quint64 totalPackets = 0;
    quint64 logicPackets = 0;
    double averageLogicMs = 0.0;
    double maxLogicMs = 0.0;
};

struct KcpMonitorStats
{
    bool listening = false;
    quint16 port = 0;
    int activeConnections = 0;
};

struct PlayerMonitorEntry
{
    quint64 clientId = 0;
    int playerId = 0;
    QString name;
    QString mapId;
    qint64 instanceId = 0;
    int partyId = 0;
    int level = 0;
    int health = 0;
    int mana = 0;
    float positionX = 0.0f;
    float positionY = 0.0f;
    bool online = false;
};

struct PartyMonitorEntry
{
    int partyId = 0;
    int leaderPlayerId = 0;
    QString leaderName;
    int memberCount = 0;
    qint64 sharedInstanceId = 0;
    QString memberNames;
};

struct DungeonMonitorEntry
{
    QString mapId;
    qint64 instanceId = 0;
    int scaledLevel = 0;
    int totalMonsterCount = 0;
    int aliveMonsterCount = 0;
    int participantCount = 0;
    int onlinePlayerCount = 0;
    bool completed = false;
};

struct ServerMonitorSnapshot
{
    QDateTime capturedAt;
    ServerModuleStatus modules;
    TcpMonitorStats tcp;
    KcpMonitorStats kcp;
    int trackedPlayerCount = 0;
    int onlineTrackedPlayerCount = 0;
    int partyCount = 0;
    int pendingInviteCount = 0;
    int dungeonRoomCount = 0;
    int activeDungeonRoomCount = 0;
    QVector<PlayerMonitorEntry> players;
    QVector<PartyMonitorEntry> parties;
    QVector<DungeonMonitorEntry> dungeons;
};

#endif // SERVERMONITORTYPES_H
