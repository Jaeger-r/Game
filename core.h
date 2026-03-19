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
#include "packdef.h"
#include "combatbalance.h"
#include <QTimer>
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
    int player_UserId;
    QString playerName;
    int level;
    long long exp;
    long long startExp;
    int health;
    int attackPower;
    int defense;
    float critRate;
    float critDamage;
    float attackRange;
    float positionX;
    float positionY;

    Player_Information(int id,
                       const QString& name,
                       int lvl,
                       long long curExp,
                       long long baseExp,
                       int hp,
                       int atk,
                       int def,
                       float critR,
                       float critDmg,
                       float atkRange,
                       float px,
                       float py)
        : player_UserId(id),
        playerName(name),
        level(lvl),
        exp(curExp),
        startExp(baseExp),
        health(hp),
        attackPower(atk),
        defense(def),
        critRate(critR),
        critDamage(critDmg),
        attackRange(atkRange),
        positionX(px),
        positionY(py)
    {}
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
    void dealData(quint64 clientId, QByteArray data);
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
    bool ensureEquipmentColumns();
    bool ensureEquipmentStateTable();
    bool ensureProgressStateTable();
public:
    bool LevelUp(int& lvl,long long& userExp,int player_id);
public:
    static ICore * getKernel(){
        if (!m_pcore) m_pcore = new core();
        return m_pcore;
    }
signals:
    void sendToClient(quint64 clientId, QByteArray data);
    void sendToClientKcp(quint64 clientId, QByteArray data);
private:
    QTimer *dazuoTimer;
    TCPNet *m_pTCPNet;
    KcpNet *m_pKcpNet;
    CMySql m_pSQL;
    static core *m_pcore;
    char m_szSystemPath[MAXSIZE];
    std::vector<Player_Information>players;
    ofstream outFile;

    std::unordered_map<int, QTimer*> m_mapDazuoTimer;
    std::unordered_map<int, std::shared_ptr<Player_Information>> m_mapPlayerInfo;
private:
    long long getEXP(int level){
        return CombatBalance::playerStats(level).expToNext;
    }
    double getHP(int level) {
        return CombatBalance::playerStats(level).maxHealth;
    }

    double getATK(int level) {
        return CombatBalance::playerStats(level).attack;
    }

    double getDEF(int level) {
        return CombatBalance::playerStats(level).defence;
    }
};

#endif // CORE_H
