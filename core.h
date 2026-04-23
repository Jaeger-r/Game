#ifndef CORE_H
#define CORE_H
#include "Icore.h"
#include "server2/tcpnet.h"
#include "server2/kcpnet.h"
#include "mysql/CMySql.h"
#include "mysql/mysqlconnpool.h"
#include <iostream>
#include <map>
#include <vector>
#include <fstream>
#include <string>
#include <ctime>
#include <algorithm>
#include <array>
#include "packdef.h"
#include "combatbalance.h"
#include "serverconfig.h"
#include "servermonitortypes.h"
#include <QTimer>
#include <QVector>
#include <QHash>
#include <QSet>
#include <QPointF>
#include <QRectF>
#include <math.h>
#include "packetbuilder.h"
#include <QCoreApplication>

using namespace std;
enum PlayerState{Normal,Ghost};
enum RegisterError
{
    REG_OK = 0,
    REG_SELECT_USEREXIT_FAIL,
    REG_SELECT_USERNAME_FAIL,
    REG_USEREXIT_FAIL,
    REG_USERNAME_FAIL,
    REG_INSERT_FAIL
};

struct Player_Information
{
    quint64 clientId;
    int player_UserId;
    QString playerName;
    QString mapId;
    qint64 instanceId;
    int level;
    long long exp;
    long long startExp;
    int health;
    int mana;
    int attackPower;
    int magicAttack;
    int independentAttack;
    int defense;
    int magicDefense;
    int strength;
    int intelligence;
    int vitality;
    int spirit;
    float critRate;
    float magicCritRate;
    float critDamage;
    float attackSpeed;
    float moveSpeed;
    float castSpeed;
    float attackRange;
    float positionX;
    float positionY;
    int direction;
    QHash<QString, int> skillLevels;
    QVector<QPair<int, int>> bagEntries;
    bool bagLoaded = false;
    qint64 authoritativeBagUpdatedAtMs = 0;
    QVector<QPair<int, int>> warehouseEntries;
    bool warehouseLoaded = false;
    int warehouseUnlockTier = 1;
    std::array<int, MAX_EQUIPMENT_SLOT_NUM> equippedItemIds{};
    std::array<int, MAX_EQUIPMENT_SLOT_NUM> equippedEnhanceLevels{};
    std::array<int, MAX_EQUIPMENT_SLOT_NUM> equippedForgeLevels{};
    std::array<int, MAX_EQUIPMENT_SLOT_NUM> equippedEnchantKinds{};
    std::array<int, MAX_EQUIPMENT_SLOT_NUM> equippedEnchantValues{};
    std::array<int, MAX_EQUIPMENT_SLOT_NUM> equippedEnhanceSuccessCounts{};
    std::array<int, MAX_EQUIPMENT_SLOT_NUM> equippedForgeSuccessCounts{};
    std::array<int, MAX_EQUIPMENT_SLOT_NUM> equippedEnchantSuccessCounts{};

    Player_Information(int id,
                       const QString& name,
                       int lvl,
                       long long curExp,
                       long long baseExp,
                       int hp,
                       int mp,
                       int atk,
                       int matk,
                       int indepAtk,
                       int def,
                       int mdef,
                       int str,
                       int intl,
                       int vit,
                       int spr,
                       float critR,
                       float magicCrit,
                       float critDmg,
                       float atkSpeed,
                       float mvSpeed,
                       float csSpeed,
                       float atkRange,
                       float px,
                       float py,
                       quint64 cid = 0,
                       const QString& map = QString(),
                       qint64 inst = 0,
                       int dir = 3)
        : clientId(cid),
        player_UserId(id),
        playerName(name),
        mapId(map.trimmed().isEmpty() ? QStringLiteral("BornWorld") : map.trimmed()),
        instanceId(inst < 0 ? 0 : inst),
        level(lvl),
        exp(curExp),
        startExp(baseExp),
        health(hp),
        mana(mp),
        attackPower(atk),
        magicAttack(matk),
        independentAttack(indepAtk),
        defense(def),
        magicDefense(mdef),
        strength(str),
        intelligence(intl),
        vitality(vit),
        spirit(spr),
        critRate(critR),
        magicCritRate(magicCrit),
        critDamage(critDmg),
        attackSpeed(atkSpeed),
        moveSpeed(mvSpeed),
        castSpeed(csSpeed),
        attackRange(atkRange),
        positionX(px),
        positionY(py),
        direction(dir)
    {}
};

struct MonsterSpawnDefinition
{
    int runtimeId = 0;
    QString displayName;
    QPointF spawnPos;
    int maxHealth = 1;
    int attack = 0;
    float aggroRange = 220.0f;
    float leashRange = 380.0f;
    float moveSpeed = 2.6f;
    float attackRange = 28.0f;
    int attackIntervalMs = 900;
    int respawnMs = 5000;
    int monsterLevel = 1;
    CombatBalance::MonsterTier monsterTier = CombatBalance::MonsterTier::Normal;
    float dropGoldRate = 1.0f;
    float dropMaterialRate = 1.0f;
    float dropEquipmentRate = 1.0f;
    int dropSetLevel = 0;
};

struct ParticipationRecord
{
    qint64 lastUpdatedAtMs = 0;
    int totalDamage = 0;
    int hitCount = 0;
};

struct MonsterState
{
    int runtimeId = 0;
    QPointF position;
    int health = 1;
    int maxHealth = 1;
    int attack = 0;
    bool alive = true;
    bool chasing = false;
    qint64 nextAttackAllowedMs = 0;
    qint64 respawnAtMs = 0;
    QHash<int, ParticipationRecord> contributorUpdatedAtMs;
};

struct DungeonRoomState
{
    QString mapId;
    qint64 instanceId = 0;
    QHash<int, MonsterSpawnDefinition> spawns;
    QVector<int> runtimeOrder;
    QHash<int, MonsterState> monsters;
    int scaledLevel = 1;
    int totalMonsterCount = 0;
    int aliveMonsterCount = 0;
    bool completed = false;
    quint16 clearVersion = 0;
    quint16 settledClearVersion = 0;
    bool dirty = true;
    qint64 lastBroadcastMs = 0;
    qint64 emptySinceMs = 0;
    QHash<int, ParticipationRecord> participantUpdatedAtMs;
};

struct ServerSkillHitWindow
{
    QString mapId;
    qint64 instanceId = 0;
    QString skillId;
    QRectF rect;
    bool persistent = false;
    qint64 expiresAtMs = 0;
};

struct PendingPartyInviteState
{
    int inviterPlayerId = 0;
    QString inviterName;
    int partyId = 0;
    qint64 expiresAtMs = 0;
};

struct PartyRuntimeState
{
    int partyId = 0;
    int leaderPlayerId = 0;
    qint64 sharedInstanceId = 0;
    QVector<int> memberPlayerIds;
};

class TCPNet;
class core : public ICore
{
    Q_OBJECT
private:
    core(QObject* parent = nullptr);
    ~core();
public:
    virtual bool open();
    virtual void close();
    void dealData(quint64 clientId, const QByteArray& data);
    void applyRuntimeConfig(const ServerRuntimeConfig& config);
public:
    void Test_Request(quint64 clientId,STRU_TEST_RQ* rq);
    void KcpNegotiate_Request(quint64 clientId, STRU_KCP_NEGOTIATE_RQ* rq);

    void Sendmessage_Request(quint64 clientId,STRU_CHAT_RQ* rq);

    void Register_Request(quint64 clientId,STRU_REGISTER_RQ* rq);
    RegisterError DoRegister(STRU_REGISTER_RQ* rq);
    void Login_Request(quint64 clientId,STRU_LOGIN_RQ* rq);
    void Initialize_Request(quint64 clientId,STRU_INITIALIZE_RQ* rq);
    void Location_Request(quint64 clientId,STRU_LOCATION_RQ* rq);
    void Save_Request(quint64 clientId,STRU_SAVE_RQ* rq);
    void Dazuo_Request(quint64 clientId,STRU_DAZUO_RQ* rq);
    void HandleAttack(quint64 clientId,STRU_ATTACK_RQ* rq);
    void RelaySkillEffect(quint64 clientId, STRU_SKILLFX_RQ* rq);
    void HandleMonsterHit(quint64 clientId, STRU_MONSTER_HIT_RQ* rq);
    void HandleSkillStateSync(quint64 clientId, STRU_SKILLSTATE_RQ* rq);
    void HandlePartyAction(quint64 clientId, STRU_PARTY_ACTION_RQ* rq);
    void HandleInventoryAction(quint64 clientId, STRU_INVENTORY_ACTION_RQ* rq);
    void InitializeBag_Request(quint64 clientId,STRU_INITBAG_RQ* rq);
    void PlayerList_Request(quint64 clientId,STRU_PLAYERLIST_RQ* rq);
public:
    bool CheckDB()
    {
        MySqlConnGuard guard;
        MYSQL* conn = guard.get();

        return conn != nullptr;
    }
    void ItemInDB();
    int getHealth(int userId);
    int calculateDamage(int attackedId,int targetId,int clientDamage);
    //PlayerAttributes getPlayerAttributesFromDB(int userId);
    bool updateHealthInDB(int userId, int newHealth);
    bool updatePlayerInDB(int userId, const Player_Information &attr);
    bool ensureUserCredentialSchema();
    bool ensurePlayerStatColumns();
    bool ensureEquipmentColumns();
    bool ensureEquipmentStateTable();
    bool ensureProgressStateTable();
    bool ensureWarehouseStateTable();
    ServerMonitorSnapshot monitorSnapshot() const;
    ServerModuleStatus moduleStatus() const;
public:
    bool LevelUp(int& lvl,long long& userExp,int player_id);
public:
    static ICore * getKernel(){
        if (!m_pcore) m_pcore = new core();
        return m_pcore;
    }
signals:
    void sendToClient(quint64 clientId, const QByteArray& data);
    void sendToClientKcp(quint64 clientId, const QByteArray& data);
private:
    QTimer *dazuoTimer;
    QString m_listenHost = QStringLiteral("127.0.0.1");
    quint16 m_tcpPort = TCP_PORT_IMPORTANT_DATA;
    quint16 m_kcpPort = KCP_PORT_REALTIME_MOVE;
    QString m_sharedDataDirectory;
    TCPNet *m_pTCPNet;
    KcpNet *m_pKcpNet;
    CMySql m_pSQL;
    static core *m_pcore;
    char m_szSystemPath[MAXSIZE];
    std::vector<Player_Information>players;
    ofstream outFile;
    QTimer* m_dungeonTickTimer = nullptr;
    QHash<QString, DungeonRoomState> m_dungeonRooms;
    QHash<QString, ServerSkillHitWindow> m_skillHitWindows;
    QHash<int, PartyRuntimeState> m_partyStates;
    QHash<int, int> m_playerPartyIds;
    QHash<int, PendingPartyInviteState> m_pendingPartyInvites;
    QHash<quint64, int> m_clientPlayerBindings;
    int m_nextPartyId = 1;
    qint64 m_nextPartySharedInstanceId = 100000;
    ServerModuleStatus m_moduleStatus;

    std::unordered_map<int, QTimer*> m_mapDazuoTimer;
    std::unordered_map<int, std::shared_ptr<Player_Information>> m_mapPlayerInfo;
private:
    Player_Information* findTrackedPlayer(int userId);
    const Player_Information* findTrackedPlayer(int userId) const;
    Player_Information* findTrackedPlayerByName(const QString& playerName);
    const Player_Information* findTrackedPlayerByName(const QString& playerName) const;
    Player_Information* findTrackedPlayerByClientId(quint64 clientId);
    const Player_Information* findTrackedPlayerByClientId(quint64 clientId) const;
    int boundPlayerIdForClient(quint64 clientId) const;
    bool isClientBoundToPlayer(quint64 clientId, int playerId) const;
    void bindClientToPlayer(quint64 clientId, int playerId);
    void clearClientBinding(quint64 clientId);
    Player_Information* findBoundTrackedPlayer(quint64 clientId, int expectedPlayerId = 0);
    const Player_Information* findBoundTrackedPlayer(quint64 clientId, int expectedPlayerId = 0) const;
    PartyRuntimeState* findPartyById(int partyId);
    const PartyRuntimeState* findPartyById(int partyId) const;
    PartyRuntimeState* findPartyByPlayerId(int playerId);
    const PartyRuntimeState* findPartyByPlayerId(int playerId) const;
    bool isPartySharedMap(const QString& mapId) const;
    bool isPartyDungeonMap(const QString& mapId) const;
    qint64 resolveAuthoritativeInstanceIdForPlayerMap(int playerId,
                                                      const QString& mapId,
                                                      qint64 fallbackInstanceId) const;
    void sendPartyStateToPlayer(int playerId,
                                quint16 action,
                                int result,
                                const QString& message,
                                bool includePendingInvite = true);
    void broadcastPartyState(int partyId,
                             quint16 action,
                             int result,
                             const QString& message,
                             int excludedPlayerId = 0);
    void clearPendingInvitesForParty(int partyId);
    void sendPartyDungeonEntryToPlayer(int playerId,
                                       const PartyRuntimeState& party,
                                       const QString& mapId,
                                       qint64 instanceId,
                                       float x,
                                       float y,
                                       const QString& message);
    void broadcastPartyDungeonEntry(int initiatorPlayerId,
                                    const QString& mapId,
                                    qint64 instanceId,
                                    float x,
                                    float y);
    void pruneExpiredSkillHitWindows(qint64 nowMs);
    void upsertTrackedPlayer(quint64 clientId,
                             int userId,
                             const QString& playerName,
                             const QString& mapId,
                             qint64 instanceId,
                             float x,
                             float y,
                             int dir = 3);
    void handleClientDisconnected(quint64 clientId);
    void broadcastScopedLocationSnapshot(const QString& mapId, qint64 instanceId);
    QVector<quint64> collectScopedClientIds(const QString& mapId, qint64 instanceId) const;
    QVector<Player_Information*> collectScopedPlayers(const QString& mapId, qint64 instanceId);
    void noteDungeonParticipant(DungeonRoomState& room, int playerId, qint64 nowMs, int damage);
    QVector<Player_Information*> collectEligibleRewardRecipients(const DungeonRoomState& room,
                                                                 int primaryPlayerId,
                                                                 const QHash<int, ParticipationRecord>& participantUpdatedAtMs,
                                                                 qint64 nowMs,
                                                                 qint64 participationWindowMs,
                                                                 int minimumDamage,
                                                                 int minimumHitCount);
    QString roomKeyFor(const QString& mapId, qint64 instanceId) const;
    QString resolveMonsterMapPath(const QString& mapId) const;
    QVector<MonsterSpawnDefinition> loadMonsterSpawnDefinitions(const QString& mapId) const;
    int resolveDungeonPartyLevelCeiling(const QString& mapId, qint64 instanceId) const;
    int resolveDungeonMonsterLevel(const DungeonRoomState& room, const MonsterSpawnDefinition& spawn) const;
    void refreshDungeonRoomScaling(DungeonRoomState& room);
    DungeonRoomState* ensureDungeonRoom(const QString& mapId, qint64 instanceId);
    void resetDungeonRoomIfNoActivePlayers(const QString& mapId, qint64 instanceId);
    bool refreshDungeonRoomProgress(DungeonRoomState& room);
    void settleDungeonRoom(DungeonRoomState& room);
    QByteArray buildDungeonStatePacket(const DungeonRoomState& room) const;
    QByteArray buildDungeonRoomPacket(const DungeonRoomState& room) const;
    void sendRealtimePacket(quint64 clientId, const QByteArray& packet);
    void sendDungeonRoomSnapshotPackets(const QVector<quint64>& clientIds,
                                        const QByteArray& roomPacket,
                                        const QByteArray& statePacket);
    void syncDungeonStateToClient(quint64 clientId, const QString& mapId, qint64 instanceId);
    void syncDungeonRoomToClient(quint64 clientId, const QString& mapId, qint64 instanceId);
    void broadcastDungeonRoomSnapshot(const QString& mapId, qint64 instanceId);
    void tickDungeonRooms();
    int computeMonsterDamageAgainstPlayer(const Player_Information& playerInfo,
                                          int rawAttack,
                                          int monsterLevel,
                                          CombatBalance::MonsterTier tier) const;
    bool isClientOnline(quint64 clientId) const;
    long long getEXP(int level){
        return CombatBalance::playerStats(level).expToNext;
    }
    double getHP(int level) {
        return CombatBalance::playerStats(level).maxHealth;
    }

    double getMP(int level) {
        return CombatBalance::playerStats(level).maxMana;
    }

    double getATK(int level) {
        return CombatBalance::playerStats(level).attack;
    }

    double getDEF(int level) {
        return CombatBalance::playerStats(level).defence;
    }
};

#endif // CORE_H
