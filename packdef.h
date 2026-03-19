#ifndef PACKDEF_H
#define PACKDEF_H
#include "tou.h"
#include <iostream>
#include <vector>
#include <QtGlobal>

#define _default_protocol_heartbeat_rq           1
#define _default_protocol_heartbeat_rs           2
#define _default_protocol_kcp_negotiate_rq       3
#define _default_protocol_kcp_negotiate_rs       4
#define _default_protocol_test_rq                5
#define _default_protocol_test_rs                6

#define _default_protocol_base  10
#define _default_protocol_chat_rq                    _default_protocol_base + 1
#define _default_protocol_chat_rs                    _default_protocol_base + 2
#define _default_protocol_location_rq                _default_protocol_base + 3
#define _default_protocol_location_rs                _default_protocol_base + 4
#define _default_protocol_login_rq                   _default_protocol_base + 5
#define _default_protocol_login_rs                   _default_protocol_base + 6
#define _default_protocol_register_rq                _default_protocol_base + 7
#define _default_protocol_register_rs                _default_protocol_base + 8
#define _default_protocol_initialize_rq              _default_protocol_base + 9
#define _default_protocol_initialize_rs              _default_protocol_base + 10
#define _default_protocol_item_rq                    _default_protocol_base + 11
#define _default_protocol_item_rs                    _default_protocol_base + 12
#define _default_protocol_chathistory_rq             _default_protocol_base + 13
#define _default_protocol_chathistory_rs             _default_protocol_base + 14
#define _default_protocol_save_rq                    _default_protocol_base + 15
#define _default_protocol_save_rs                    _default_protocol_base + 16
#define _default_protocol_dazuo_rq                   _default_protocol_base + 17
#define _default_protocol_dazuo_rs                   _default_protocol_base + 18
#define _default_protocol_levelup_rq                 _default_protocol_base + 19
#define _default_protocol_levelup_rs                 _default_protocol_base + 20
#define _default_protocol_attack_rq                  _default_protocol_base + 21
#define _default_protocol_attack_rs                  _default_protocol_base + 22
#define _default_protocol_revive_rq                  _default_protocol_base + 23
#define _default_protocol_revive_rs                  _default_protocol_base + 24
#define _default_protocol_initbag_rq                 _default_protocol_base + 25
#define _default_protocol_initbag_rs                 _default_protocol_base + 26
#define _default_protocol_playerlist_rq              _default_protocol_base + 27
#define _default_protocol_playerlist_rs              _default_protocol_base + 28

//#define UDPSERVER
#define MAXSIZE 1024
#define NAMESIZE 50
#define MAPID_SIZE 64
#define MAX_BAG_ITEM_NUM 48
#define MAX_EQUIPMENT_SLOT_NUM 7

#define _sql_error_        0001

#define _register_success_         1000
#define _register_err_             1001
#define _register_password_error   1002
#define _register_userexist_        1003
#define _register_nameexist_        1004

#define _login_usernoexist 1100
#define _login_passworderr 1101
#define _login_success     1102
#define _login_error       1104

#define _initialize_success 1200
#define _initialize_error   1201
#define _initialize_fail    1202

#define _initializebag_success 1203
#define _initializebag_error   1204
#define _initializebag_fail    1205


#define _item_success 1300
#define _item_error   1301
#define _item_fail    1302

#define _save_success_ 1400
#define _save_fail_    1401
#define _save_error_   1402

/**
 * @brief 包头
 */
#pragma pack(push,1)
struct PacketHeader {
    quint16 length;     //包长度
    quint16 cmd;        //包类型
};
#pragma pack(pop)

/**
 * @brief 测试
 */
struct STRU_TEST_RQ {
    char player_Name[NAMESIZE];
    char player_Password[NAMESIZE];
    long long player_tel;
};
struct STRU_TEST_RS {
    char player_Result;
};


/**
 * @brief KCP协商请求
 */
struct STRU_KCP_NEGOTIATE_RQ {
    //quint32 kcpConv;      // KCP会话ID
    //quint16 protoVersion;   // 协议版本
    //quint8  kcpMode;      // KCP模式: 0=Default, 1=Normal, 2=Fast
    //quint16 kcpPort;      // KCP UDP端口
};

struct STRU_KCP_NEGOTIATE_RS {
    quint16 result;          // 0=失败, 1=成功
    quint32 kcpConv;      // KCP会话ID（服务器确认）
    quint8  kcpMode;      // KCP模式（服务器确认）
    quint16 kcpPort;      // KCP UDP端口（服务器确认）
};

/**
 * @brief 心跳
 */
struct STRU_HEARTBEAT_RQ {};
struct STRU_HEARTBEAT_RS {};


/**
 * @brief 注册
 */

struct STRU_REGISTER_RQ
{
    char player_Name[NAMESIZE];
    char player_Password[NAMESIZE];
    long long player_tel;
};
struct STRU_REGISTER_RS
{
    int player_Result;
};


/**
 * @brief 登录
 */
struct STRU_LOGIN_RQ
{
    char player_Name[NAMESIZE];
    char player_Password[NAMESIZE];
};
struct STRU_LOGIN_RS
{
    int player_UserId;
    int player_Result;
};


/**
 * @brief 聊天
 */
struct STRU_CHAT_RQ
{
    char player_Name[NAMESIZE];
    char szbuf[MAXSIZE];
};
struct STRU_CHAT_RS
{
    int player_UserId;
    char player_Name[NAMESIZE];
    char szbuf[MAXSIZE];
};


/**
 * @brief 玩家属性初始化
 */
struct STRU_INITIALIZE_RQ
{
    int player_UserId;
};
struct STRU_INITIALIZE_RS
{
    char player_Name[NAMESIZE];
    int health;                   //生命值
    int attackPower;              //攻击力
    int attackRange;              //攻击范围
    long long experience;         //经验
    int level;                    //等级
    int defence;                  //防御力
    float critical_rate;          //暴击率
    float critical_damage;        //暴击伤害
    float x;                      //玩家坐标X
    float y;                      //玩家坐标Y
    char mapId[MAPID_SIZE];       //当前地图ID
    int questStep;                //任务阶段
    int Initialize_Result;       //初始化结果
    int player_UserId;
};

/**
 * @brief 实时发送玩家坐标
 */
struct PlayerLocation
{
    int player_UserId;
    char player_Name[NAMESIZE];
    float x;
    float y;
};
struct STRU_LOCATION_RQ
{
    int player_UserId;
    char player_Name[NAMESIZE];
    float x;
    float y;
};
struct STRU_LOCATION_RS
{
    int playerCount;
    PlayerLocation players[50];
};


/**
 * @brief 定时向服务器申请保存
 */
struct STRU_SAVE_RQ
{
    int player_UserId;            //玩家ID
    int health;                   //生命值
    int attackPower;              //攻击力
    int attackRange;              //攻击范围
    long long experience;         //经验
    int level;                    //等级
    int defence;                  //防御力
    float critical_rate;          //暴击率
    float critical_damage;        //暴击伤害
    float x;                      //玩家坐标X
    float y;                      //玩家坐标Y
    char mapId[MAPID_SIZE];       //当前地图ID
    int questStep;                //任务阶段
    int equippedItemIds[MAX_EQUIPMENT_SLOT_NUM];
    int equippedEnhanceLevels[MAX_EQUIPMENT_SLOT_NUM];
    int equippedForgeLevels[MAX_EQUIPMENT_SLOT_NUM];
    int equippedEnchantKinds[MAX_EQUIPMENT_SLOT_NUM];
    int equippedEnchantValues[MAX_EQUIPMENT_SLOT_NUM];
};
struct STRU_SAVE_RS
{
    int Save_Result;
};


/**
 * @brief 打坐申请
 */
struct STRU_DAZUO_RQ
{
    int player_UserId;
    bool isDazuo;
};
struct STRU_DAZUO_RS
{
    long long exp;
    int userlevel;
    long long addedExp;
    long long MaxExp;
    bool dazuoConfirm;
};


/**
 * @brief 升级协议
 */
struct STRU_LEVELUP_RQ
{
    int player_UserId;
};
struct STRU_LEVELUP_RS
{
    long long MaxExp;
    int userlevel;
};


/**
 * @brief 攻击包
 */
struct STRU_ATTACK_RQ
{
    int attackerId;  // 攻击者ID
    int targetId;    // 被攻击玩家ID
    float damage;    // 客户端计算的伤害值（用于校验）
};

struct STRU_ATTACK_RS
{
    int targetId;
    int currentHealth; // 更新后的血量
    bool isDead;       // 是否死亡
};


/**
 * @brief 重生协议
 */
struct STRU_REVIVE_RQ{};
struct STRU_REVIVE_RS
{
    int playerId;
    int newhealth;
};


/**
 * @brief 初始化背包
 */
struct STRU_INITBAG_RQ
{
    int playerId;
};
struct STRU_INITBAG_RS
{
    int itemAmount;
    int Result;
    int playerBag[MAX_BAG_ITEM_NUM][2];
    int equippedItemIds[MAX_EQUIPMENT_SLOT_NUM];
    int equippedEnhanceLevels[MAX_EQUIPMENT_SLOT_NUM];
    int equippedForgeLevels[MAX_EQUIPMENT_SLOT_NUM];
    int equippedEnchantKinds[MAX_EQUIPMENT_SLOT_NUM];
    int equippedEnchantValues[MAX_EQUIPMENT_SLOT_NUM];
};


/**
 * @brief 获取玩家列表
 */
struct STRU_PLAYERLIST_RQ
{
    int playerId;
};
struct STRU_PLAYERLIST_RS
{


};
#endif // PACKDEF_H
