#include "core.h"
#include <QDateTime>
#include <QTextStream>
#include <QDebug>
#include "QtKcp/src/qkcpsocket.h"

#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define RESET   "\033[0m"

core *core::m_pcore = nullptr;
core::core(QObject* parent)
    : ICore(parent)
{
    JaegerDebug();
    m_pTCPNet = new TCPNet(this);
    m_pKcpNet = new KcpNet(this);

    // 设置TCPNet的KcpNet指针
    m_pTCPNet->setKcpNet(m_pKcpNet);
    // 连接KCP数据接收信号到dealData
    connect(m_pKcpNet, &KcpNet::kcpDataReceived,
            this, [this](quint64 clientId, QByteArray data) {
                // KCP数据直接交给dealData处理
                dealData(clientId, data);
            });

}

core:: ~core()
{
    //JaegerDebug();
    delete m_pTCPNet;
    delete m_pKcpNet;
}

bool core::open() {
    QTextStream out(stdout); // 也可以用 qDebug().noquote()

    bool tcpOk = false;
    bool kcpOk = false;
    bool dbOk = false;
    bool fileOk = false;

    out << "\n===== Initializing Server Modules =====\n";

    // ---------------- TCP ----------------
    out << "-> TCP Network: ";
    tcpOk = m_pTCPNet->initNetWork("127.0.0.1", TCP_PORT_IMPORTANT_DATA);
    out << (tcpOk ? GREEN "OK" RESET : RED "FAILED" RESET)
        << " (Port: " << TCP_PORT_IMPORTANT_DATA << ")\n";

    // ---------------- KCP ----------------
    out << "-> KCP Network: ";
    kcpOk = m_pKcpNet->initNetWork("127.0.0.1", KCP_PORT_REALTIME_MOVE);
    out << (kcpOk ? GREEN "OK" RESET : RED "FAILED" RESET)
        << " (Port: " << KCP_PORT_REALTIME_MOVE << ")\n";

    // ---------------- DB ----------------
    out << "-> Database: ";
    dbOk = CheckDB();
    if (dbOk) {
        dbOk = ensurePlayerStatColumns();
    }
    if (dbOk) {
        dbOk = ensureEquipmentColumns();
    }
    if (dbOk) {
        dbOk = ensureEquipmentStateTable();
    }
    if (dbOk) {
        dbOk = ensureProgressStateTable();
    }
    out << (dbOk ? GREEN "OK" RESET : RED "FAILED" RESET) << "\n";

    // ---------------- File ----------------
    out << "-> Chat History File: ";
    outFile.open("chat_history.txt", std::ios::app);
    fileOk = outFile.is_open();
    out << (fileOk ? GREEN "OK" RESET : RED "FAILED" RESET) << "\n";

    // ---------------- Summary ----------------
    out << "---------------------------------------\n";
    if (tcpOk && kcpOk && dbOk && fileOk) {
        out << GREEN "All modules initialized successfully!" RESET << "\n";
        return true;
    } else {
        out << RED "One or more modules failed to initialize!" RESET << "\n";
        return false;
    }
}

bool core::ensureEquipmentColumns()
{
    CMySql sql;
    const std::vector<std::pair<const char*, const char*>> columns = {
        {"u_equipment_weapon", "INT NOT NULL DEFAULT 0"},
        {"u_equipment_head", "INT NOT NULL DEFAULT 0"},
        {"u_equipment_body", "INT NOT NULL DEFAULT 0"},
        {"u_equipment_legs", "INT NOT NULL DEFAULT 0"},
        {"u_equipment_hands", "INT NOT NULL DEFAULT 0"},
        {"u_equipment_shoes", "INT NOT NULL DEFAULT 0"},
        {"u_equipment_shield", "INT NOT NULL DEFAULT 0"}
    };

    char szsql[MAXSIZE * 2] = {0};
    for (const auto& column : columns) {
        std::list<std::string> result;
        snprintf(szsql,
                 sizeof(szsql),
                 "SELECT COUNT(*) FROM information_schema.COLUMNS "
                 "WHERE TABLE_SCHEMA = DATABASE() "
                 "AND TABLE_NAME = 'user_basic_information' "
                 "AND COLUMN_NAME = '%s';",
                 column.first);

        if (!sql.SelectMySql(szsql, 1, result)) {
            return false;
        }

        const bool exists = !result.empty() && result.front() != "0";
        if (exists) {
            continue;
        }

        snprintf(szsql,
                 sizeof(szsql),
                 "ALTER TABLE user_basic_information "
                 "ADD COLUMN %s %s;",
                 column.first,
                 column.second);

        if (!sql.UpdateMySql(szsql)) {
            return false;
        }
    }

    return true;
}

bool core::ensurePlayerStatColumns()
{
    CMySql sql;
    const std::vector<std::pair<const char*, const char*>> columns = {
        {"u_mana", "INT NOT NULL DEFAULT 0"},
        {"u_magicattack", "INT NOT NULL DEFAULT 0"},
        {"u_independentattack", "INT NOT NULL DEFAULT 0"},
        {"u_magicdefence", "INT NOT NULL DEFAULT 0"},
        {"u_strength", "INT NOT NULL DEFAULT 0"},
        {"u_intelligence", "INT NOT NULL DEFAULT 0"},
        {"u_vitality", "INT NOT NULL DEFAULT 0"},
        {"u_spirit", "INT NOT NULL DEFAULT 0"},
        {"u_magiccritrate", "FLOAT NOT NULL DEFAULT 0"},
        {"u_attackspeed", "FLOAT NOT NULL DEFAULT 0"},
        {"u_movespeed", "FLOAT NOT NULL DEFAULT 0"},
        {"u_castspeed", "FLOAT NOT NULL DEFAULT 0"}
    };

    char szsql[MAXSIZE * 2] = {0};
    for (const auto& column : columns) {
        std::list<std::string> result;
        snprintf(szsql,
                 sizeof(szsql),
                 "SELECT COUNT(*) FROM information_schema.COLUMNS "
                 "WHERE TABLE_SCHEMA = DATABASE() "
                 "AND TABLE_NAME = 'user_basic_information' "
                 "AND COLUMN_NAME = '%s';",
                 column.first);

        if (!sql.SelectMySql(szsql, 1, result)) {
            return false;
        }

        const bool exists = !result.empty() && result.front() != "0";
        if (exists) {
            continue;
        }

        snprintf(szsql,
                 sizeof(szsql),
                 "ALTER TABLE user_basic_information "
                 "ADD COLUMN %s %s;",
                 column.first,
                 column.second);

        if (!sql.UpdateMySql(szsql)) {
            return false;
        }
    }

    return true;
}

bool core::ensureEquipmentStateTable()
{
    CMySql sql;
    const char* createSql =
        "CREATE TABLE IF NOT EXISTS user_equipment_state ("
        "u_id INT NOT NULL, "
        "slot_index INT NOT NULL, "
        "item_id INT NOT NULL DEFAULT 0, "
        "enhance_level INT NOT NULL DEFAULT 0, "
        "forge_level INT NOT NULL DEFAULT 0, "
        "enchant_kind INT NOT NULL DEFAULT 0, "
        "enchant_value INT NOT NULL DEFAULT 0, "
        "PRIMARY KEY (u_id, slot_index)"
        ");";
    return sql.UpdateMySql(createSql);
}

bool core::ensureProgressStateTable()
{
    CMySql sql;
    const char* createSql =
        "CREATE TABLE IF NOT EXISTS user_progress_state ("
        "u_id INT NOT NULL PRIMARY KEY, "
        "map_id VARCHAR(64) NOT NULL DEFAULT 'BornWorld', "
        "quest_step INT NOT NULL DEFAULT 0, "
        "pos_x FLOAT NOT NULL DEFAULT 100, "
        "pos_y FLOAT NOT NULL DEFAULT 100, "
        "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP"
        ");";
    return sql.UpdateMySql(createSql);
}

void core::close(){
    //JaegerDebug();
    if (m_pTCPNet)
    {
        m_pTCPNet->unInitNetWork();
    }
    if (m_pKcpNet)
    {
        m_pKcpNet->unInitNetWork();
    }
    outFile.close();
}

void core::dealData(quint64 clientId, QByteArray data)
{
    PacketHeader* header = (PacketHeader*)data.constData();
    const char* body = data.constData() + sizeof(PacketHeader);

    switch(header->cmd){
    case _default_protocol_test_rq:
        Test_Request(clientId,(STRU_TEST_RQ*)body);
        break;
    case _default_protocol_kcp_negotiate_rq:
        KcpNegotiate_Request(clientId, (STRU_KCP_NEGOTIATE_RQ*)body);
        break;
    case _default_protocol_login_rq:
        Login_Request(clientId,(STRU_LOGIN_RQ*)body);
        break;
    case _default_protocol_register_rq:
        Register_Request(clientId,(STRU_REGISTER_RQ*)body);
        break;
    case _default_protocol_initialize_rq:
        Initialize_Request(clientId,(STRU_INITIALIZE_RQ*)body);
        break;
    case _default_protocol_location_rq:
        Location_Request(clientId,(STRU_LOCATION_RQ*)body);
        break;
    case _default_protocol_chat_rq:
        Sendmessage_Request(clientId,(STRU_CHAT_RQ*)body);
        break;
    case _default_protocol_save_rq:
        Save_Request(clientId,(STRU_SAVE_RQ*)body);
        break;
    case _default_protocol_dazuo_rq:
        Dazuo_Request(clientId,(STRU_DAZUO_RQ*)body);
        break;
    case _default_protocol_attack_rq:
        HandleAttack(clientId,(STRU_ATTACK_RQ*)body);
        break;
    case _default_protocol_initbag_rq:
        InitializeBag_Request(clientId,(STRU_INITBAG_RQ*)body);
        break;
    case _default_protocol_playerlist_rq:
        PlayerList_Request(clientId,(STRU_PLAYERLIST_RQ*)body);
        break;
    }
}

void core::Test_Request(quint64 clientId, STRU_TEST_RQ* rq)
{
    JaegerDebug();
    qDebug() << "ClientId:" << clientId;
    qDebug() << "PlayerName:" << rq->player_Name;
    qDebug() << "PlayerPassWord:" << rq->player_Password;
    qDebug() << "PlayerTel:" << rq->player_tel;

    STRU_TEST_RS test_rs;

    bool ok =
        strlen(rq->player_Name) > 0 &&
        strlen(rq->player_Password) > 0 &&
        rq->player_tel > 0;

    test_rs.player_Result = ok ? 1 : 0;

    QByteArray packet = PacketBuilder::build(_default_protocol_test_rs,test_rs);
    emit sendToClient(clientId, packet);
}

void core::KcpNegotiate_Request(quint64 clientId, STRU_KCP_NEGOTIATE_RQ* rq)
{
    Q_UNUSED(rq);
    qDebug() << "KCP Negotiate Request from ClientID:" << clientId;

    STRU_KCP_NEGOTIATE_RS rs;
    rs.result = 0; // 默认失败
    rs.kcpConv = 0;
    rs.kcpPort = 0;
    rs.kcpMode = (quint8)QKcpSocket::Mode::Fast;

    QTcpSocket* tcpSocket = m_pTCPNet->getSocket(clientId);
    if (m_pKcpNet->handleKcpNegotiate(clientId, tcpSocket, rs.kcpConv, rs.kcpPort)) {
        rs.result = 1; // 成功
        qDebug() << "KCP Negotiate SUCCESS for ClientID:" << clientId << "Conv:" << rs.kcpConv << "Port:" << rs.kcpPort;
    } else {
        qDebug() << "KCP Negotiate FAILED for ClientID:" << clientId;
    }

    QByteArray sendDataArray = PacketBuilder::build(_default_protocol_kcp_negotiate_rs, rs);
    emit sendToClient(clientId, sendDataArray); // 通过TCP回包
}

void core::Sendmessage_Request(quint64 clientId, STRU_CHAT_RQ* rq){
    JaegerDebug();
    STRU_CHAT_RS chat_rs;

    // 获取当前时间
    time_t now = time(nullptr);
    tm* localTime = localtime(&now);
    char timeStr[128];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localTime);

    // 复制玩家信息，防止溢出
    strncpy(chat_rs.player_Name, rq->player_Name, sizeof(chat_rs.player_Name) - 1);
    chat_rs.player_Name[sizeof(chat_rs.player_Name) - 1] = '\0';  // 确保字符串结尾
    strncpy(chat_rs.szbuf, rq->szbuf, sizeof(chat_rs.szbuf) - 1);
    chat_rs.szbuf[sizeof(chat_rs.szbuf) - 1] = '\0';  // 确保字符串结尾

    // 遍历 QMap 进行广播
    const auto& clientIds = m_pTCPNet->getAllClientIds();
    for (quint64 clientId : clientIds) {
        QByteArray packet = PacketBuilder::build(_default_protocol_chat_rs,chat_rs);
        emit sendToClient(clientId, packet);
    }

    // 记录聊天内容到日志文件
    std::cout << "[System Record] User " << chat_rs.player_Name << ": " << chat_rs.szbuf << std::endl;

    std::ofstream outFile("chat_history.txt", std::ios::app);  // 追加模式打开文件
    if (outFile.is_open()) {
        outFile << "[" << timeStr << "] User " << chat_rs.player_Name << ": " << chat_rs.szbuf << "\n";
        outFile.flush();
    } else {
        std::cerr << "Failed to open chat log file." << std::endl;
    }
}
RegisterError core::DoRegister(STRU_REGISTER_RQ* rq)
{
    CMySql mysql;
    char sql[MAXSIZE]{0};
    list<string> result;
    const int baseLevel = 1;
    const CombatBalance::PlayerStats baseStats = CombatBalance::playerStats(baseLevel);

    //查手机号
    sprintf(sql,
            "select u_id from user where u_tel=%lld;",
            rq->player_tel);

    if (!mysql.SelectMySql(sql,1,result))
        return REG_SELECT_USEREXIT_FAIL;

    if (!result.empty())
        return REG_USEREXIT_FAIL;

    result.clear();

    //查用户名
    sprintf(sql,
            "select u_id from user where u_name='%s';",
            rq->player_Name);

    if (!mysql.SelectMySql(sql,1,result))
        return REG_SELECT_USERNAME_FAIL;

    if (!result.empty())
        return REG_USERNAME_FAIL;

    //插入用户
    sprintf(sql,
            "insert into user(u_name,u_password,u_tel) values('%s','%s','%lld');",
            rq->player_Name,
            rq->player_Password,
            rq->player_tel);

    if (!mysql.UpdateMySql(sql))
        return REG_INSERT_FAIL;
    result.clear();
    //获取ID，新增其他信息
    sprintf(sql,
            "select u_id from user where u_name='%s';",
            rq->player_Name);

    if (!mysql.SelectMySql(sql,1,result))
        return REG_SELECT_USERNAME_FAIL;
    int user_id = std::stoi(result.front());
    sprintf(sql,
            "insert into user_basic_information("
            "u_id,u_name,u_health,u_mana,u_attackpower,u_magicattack,u_independentattack,"
            "u_attackrange,u_experience,u_level,u_defence,u_magicdefence,"
            "u_strength,u_intelligence,u_vitality,u_spirit,"
            "u_critrate,u_magiccritrate,u_critdamage,"
            "u_attackspeed,u_movespeed,u_castspeed,u_position_x,u_position_y) "
            "values (%d,'%s','%d','%d','%d','%d','%d','%d','%d','%d','%d','%d','%d','%d','%d','%d','%.3f','%.3f','%.3f','%.3f','%.3f','%.3f','%.1f','%.1f');",
            user_id,
            rq->player_Name,
            baseStats.maxHealth,
            baseStats.maxMana,
            baseStats.attack,
            baseStats.magicAttack,
            baseStats.independentAttack,
            baseStats.attackRange,
            0,
            baseLevel,
            baseStats.defence,
            baseStats.magicDefence,
            baseStats.strength,
            baseStats.intelligence,
            baseStats.vitality,
            baseStats.spirit,
            baseStats.critRate,
            baseStats.magicCritRate,
            baseStats.critDamage,
            baseStats.attackSpeed,
            baseStats.moveSpeed,
            baseStats.castSpeed,
            100.0f,
            100.0f
            );
    if (!mysql.UpdateMySql(sql))
        return REG_INSERT_FAIL;
    result.clear();

    const std::vector<std::pair<int, int>> starterItems = {
        {1, 1},      // 初行者长剑
        {7, 1},      // 初行者护盾
        {10, 1},     // 初行者胸甲
        {12, 1},     // 初行者头盔
        {13, 1},     // 初行者护腿
        {14, 1},     // 初行者护手
        {15, 1},     // 初行者战靴
        {8, 20},     // 红苹果
        {11, 8},     // 大红药水
        {1001, 45000}, // 金币
        {1002, 140}, // 强化石
        {1003, 90},  // 锻造锤
        {1004, 80},  // 魔力结晶
        {1005, 360}, // 无色晶块
        {1006, 130}  // 炉岩碳
    };

    for (const auto& item : starterItems) {
        snprintf(sql,
                 sizeof(sql),
                 "INSERT INTO user_item (u_id, item_id, item_count) "
                 "VALUES ('%d', '%d', '%d') "
                 "ON DUPLICATE KEY UPDATE item_count = item_count + VALUES(item_count);",
                 user_id,
                 item.first,
                 item.second);
        if (!mysql.UpdateMySql(sql)) {
            return REG_INSERT_FAIL;
        }
    }
    return REG_OK;
}
void core::Register_Request(quint64 clientId, STRU_REGISTER_RQ* rq)
{
    JaegerDebug();

    STRU_REGISTER_RS register_rs;
    register_rs.player_Result = _register_err_;

    RegisterError err = DoRegister(rq);

    switch (err)
    {
    case REG_OK:
        register_rs.player_Result = _register_success_;
        break;

    case REG_USEREXIT_FAIL:        // 手机号已注册
        register_rs.player_Result = _register_userexist_;
        break;

    case REG_USERNAME_FAIL:        // 用户名已存在
        register_rs.player_Result = _register_nameexist_;
        break;

    case REG_INSERT_FAIL:
        register_rs.player_Result = _register_err_;
        break;

    case REG_SELECT_USEREXIT_FAIL:
        register_rs.player_Result = _register_err_;
        break;

    case REG_SELECT_USERNAME_FAIL:
        register_rs.player_Result = _register_err_;
        break;
    }

    qDebug()<<err;
    QByteArray packet = PacketBuilder::build(_default_protocol_register_rs, register_rs);
    emit sendToClient(clientId, packet);
}

void core::Login_Request(quint64 clientId, STRU_LOGIN_RQ* rq)
{
    //JaegerDebug();

    CMySql mysql;
    STRU_LOGIN_RS login_rs;
    char szsql[MAXSIZE] = {0};
    list<string>liststr;
    login_rs.player_Result = _login_usernoexist;

    sprintf(szsql,"select u_id,u_password from user where u_name = '%s';",rq->player_Name);
    if (!mysql.SelectMySql(szsql,2,liststr))
        login_rs.player_Result = _login_error;

    if (!liststr.empty())
    {
        login_rs.player_Result = _login_passworderr;
        string strUserId = liststr.front();
        liststr.pop_front();
        string strUserPassword = liststr.front();
        liststr.pop_front();
        if (strcmp(rq->player_Password,strUserPassword.c_str()) == 0)
        {
            login_rs.player_Result = _login_success;
            login_rs.player_UserId = std::stoi(strUserId);
            QByteArray packet = PacketBuilder::build(_default_protocol_login_rs,login_rs);
            emit sendToClient(clientId, packet);
            //m_pTCPNet->sendData(sock,(char*)&sls,sizeof(sls));
        }
        else
        {
            login_rs.player_Result = _login_passworderr;
            login_rs.player_UserId = -1;
            QByteArray packet = PacketBuilder::build(_default_protocol_login_rs,login_rs);
            emit sendToClient(clientId, packet);
            //m_pTCPNet->sendData(sock,(char*)&sls,sizeof(sls));
        }
    }
    else
    {
        login_rs.player_Result = _login_error;
        login_rs.player_UserId = -1;
        QByteArray packet = PacketBuilder::build(_default_protocol_login_rs,login_rs);
        emit sendToClient(clientId, packet);
    }
}



void core::Initialize_Request(quint64 clientId, STRU_INITIALIZE_RQ* rq)
{
    JaegerDebug();

    ensureProgressStateTable();
    CMySql sql;
    STRU_INITIALIZE_RS initialize_rs{};
    initialize_rs.Initialize_Result = _initialize_fail;
    snprintf(initialize_rs.mapId, sizeof(initialize_rs.mapId), "%s", "BornWorld");
    initialize_rs.questStep = 0;

    auto parseInt = [](const std::string& value, int fallback) -> int {
        try {
            return std::stoi(value);
        } catch (...) {
            return fallback;
        }
    };
    auto parseLongLong = [](const std::string& value, long long fallback) -> long long {
        try {
            return std::stoll(value);
        } catch (...) {
            return fallback;
        }
    };
    auto parseFloat = [](const std::string& value, float fallback) -> float {
        try {
            return std::stof(value);
        } catch (...) {
            return fallback;
        }
    };

    char szsql[MAXSIZE * 3] = {0};
    list<string> liststr;
    snprintf(szsql,
             sizeof(szsql),
             "SELECT "
             "b.u_name, b.u_health, b.u_mana, "
             "b.u_attackpower, b.u_magicattack, b.u_independentattack, b.u_attackrange, "
             "b.u_experience, b.u_level, "
             "b.u_defence, b.u_magicdefence, "
             "b.u_strength, b.u_intelligence, b.u_vitality, b.u_spirit, "
             "b.u_critrate, b.u_magiccritrate, b.u_critdamage, "
             "b.u_attackspeed, b.u_movespeed, b.u_castspeed, "
             "b.u_position_x, b.u_position_y, "
             "COALESCE(p.map_id, 'BornWorld'), COALESCE(p.quest_step, 0), "
             "COALESCE(p.pos_x, b.u_position_x), COALESCE(p.pos_y, b.u_position_y) "
             "FROM user_basic_information b "
             "LEFT JOIN user_progress_state p ON p.u_id = b.u_id "
             "WHERE b.u_id = '%d' LIMIT 1;",
             rq->player_UserId);

    if (!sql.SelectMySql(szsql, 27, liststr) || liststr.size() < 27) {
        initialize_rs.Initialize_Result = _initialize_error;
        QByteArray packet = PacketBuilder::build(_default_protocol_initialize_rs, initialize_rs);
        emit sendToClient(clientId, packet);
        return;
    }

    string strUserName = liststr.front();
    snprintf(initialize_rs.player_Name, sizeof(initialize_rs.player_Name), "%s", strUserName.c_str());
    liststr.pop_front();

    initialize_rs.health = parseInt(liststr.front(), 100);
    liststr.pop_front();
    initialize_rs.mana = parseInt(liststr.front(), 60);
    liststr.pop_front();
    initialize_rs.attackPower = parseInt(liststr.front(), 10);
    liststr.pop_front();
    initialize_rs.magicAttack = parseInt(liststr.front(), 10);
    liststr.pop_front();
    initialize_rs.independentAttack = parseInt(liststr.front(), 10);
    liststr.pop_front();
    initialize_rs.attackRange = parseInt(liststr.front(), 30);
    liststr.pop_front();
    initialize_rs.experience = parseLongLong(liststr.front(), 0);
    liststr.pop_front();
    initialize_rs.level = parseInt(liststr.front(), 1);
    liststr.pop_front();
    initialize_rs.defence = parseInt(liststr.front(), 12);
    liststr.pop_front();
    initialize_rs.magicDefence = parseInt(liststr.front(), 10);
    liststr.pop_front();
    initialize_rs.strength = parseInt(liststr.front(), 0);
    liststr.pop_front();
    initialize_rs.intelligence = parseInt(liststr.front(), 0);
    liststr.pop_front();
    initialize_rs.vitality = parseInt(liststr.front(), 0);
    liststr.pop_front();
    initialize_rs.spirit = parseInt(liststr.front(), 0);
    liststr.pop_front();
    initialize_rs.critical_rate = parseFloat(liststr.front(), 0.05f);
    liststr.pop_front();
    initialize_rs.magic_critical_rate = parseFloat(liststr.front(), 0.04f);
    liststr.pop_front();
    initialize_rs.critical_damage = parseFloat(liststr.front(), 1.5f);
    liststr.pop_front();
    initialize_rs.attack_speed = parseFloat(liststr.front(), 0.0f);
    liststr.pop_front();
    initialize_rs.move_speed = parseFloat(liststr.front(), 0.0f);
    liststr.pop_front();
    initialize_rs.cast_speed = parseFloat(liststr.front(), 0.0f);
    liststr.pop_front();
    initialize_rs.x = parseFloat(liststr.front(), 100.0f);
    liststr.pop_front();
    initialize_rs.y = parseFloat(liststr.front(), 100.0f);
    liststr.pop_front();

    string strMapId = liststr.front();
    liststr.pop_front();
    if (strMapId.empty()) {
        strMapId = "BornWorld";
    }
    snprintf(initialize_rs.mapId, sizeof(initialize_rs.mapId), "%s", strMapId.c_str());

    initialize_rs.questStep = parseInt(liststr.front(), 0);
    liststr.pop_front();

    initialize_rs.x = parseFloat(liststr.front(), initialize_rs.x);
    liststr.pop_front();
    initialize_rs.y = parseFloat(liststr.front(), initialize_rs.y);
    liststr.pop_front();

    initialize_rs.level = CombatBalance::clampLevel(initialize_rs.level);
    initialize_rs.experience = qMax(0LL, initialize_rs.experience);

    // 统一按等级基准重建角色核心战斗属性，避免历史旧档造成数值漂移
    const CombatBalance::PlayerStats expected = CombatBalance::playerStats(initialize_rs.level);
    initialize_rs.attackPower = expected.attack;
    initialize_rs.magicAttack = expected.magicAttack;
    initialize_rs.independentAttack = expected.independentAttack;
    initialize_rs.defence = expected.defence;
    initialize_rs.magicDefence = expected.magicDefence;
    initialize_rs.strength = expected.strength;
    initialize_rs.intelligence = expected.intelligence;
    initialize_rs.vitality = expected.vitality;
    initialize_rs.spirit = expected.spirit;
    initialize_rs.critical_rate = expected.critRate;
    initialize_rs.magic_critical_rate = expected.magicCritRate;
    initialize_rs.critical_damage = expected.critDamage;
    initialize_rs.attack_speed = expected.attackSpeed;
    initialize_rs.move_speed = expected.moveSpeed;
    initialize_rs.cast_speed = expected.castSpeed;
    initialize_rs.attackRange = qMax(20, expected.attackRange);
    initialize_rs.health = qBound(1, initialize_rs.health, expected.maxHealth);
    initialize_rs.mana = qBound(0, initialize_rs.mana, expected.maxMana);

    // 将标准化后的属性同步回数据库（权威端）
    snprintf(szsql,
             sizeof(szsql),
             "UPDATE user_basic_information SET "
             "u_health = '%d', "
             "u_mana = '%d', "
             "u_attackpower = '%d', "
             "u_magicattack = '%d', "
             "u_independentattack = '%d', "
             "u_attackrange = '%d', "
             "u_defence = '%d', "
             "u_magicdefence = '%d', "
             "u_strength = '%d', "
             "u_intelligence = '%d', "
             "u_vitality = '%d', "
             "u_spirit = '%d', "
             "u_critrate = '%.3f', "
             "u_magiccritrate = '%.3f', "
             "u_critdamage = '%.3f', "
             "u_attackspeed = '%.3f', "
             "u_movespeed = '%.3f', "
             "u_castspeed = '%.3f' "
             "WHERE u_id = '%d';",
             initialize_rs.health,
             initialize_rs.mana,
             initialize_rs.attackPower,
             initialize_rs.magicAttack,
             initialize_rs.independentAttack,
             initialize_rs.attackRange,
             initialize_rs.defence,
             initialize_rs.magicDefence,
             initialize_rs.strength,
             initialize_rs.intelligence,
             initialize_rs.vitality,
             initialize_rs.spirit,
             initialize_rs.critical_rate,
             initialize_rs.magic_critical_rate,
             initialize_rs.critical_damage,
             initialize_rs.attack_speed,
             initialize_rs.move_speed,
             initialize_rs.cast_speed,
             rq->player_UserId);
    sql.UpdateMySql(szsql);

    initialize_rs.Initialize_Result = _initialize_success;
    initialize_rs.player_UserId = rq->player_UserId;
    qDebug() << "Init Success";
    QByteArray packet = PacketBuilder::build(_default_protocol_initialize_rs, initialize_rs);
    emit sendToClient(clientId, packet);
}

void core::InitializeBag_Request(quint64 clientId, STRU_INITBAG_RQ* rq)
{
    JaegerDebug();

    ensureEquipmentColumns();
    ensureEquipmentStateTable();
    CMySql sql;
    STRU_INITBAG_RS initbag_rs;
    memset(&initbag_rs, 0, sizeof(initbag_rs));
    char szsql[MAXSIZE] = {0};

    std::vector<std::vector<std::string>> result;
    std::list<std::string> liststr;

    qDebug() << "InitBag: querying for player" << rq->playerId;

    // 查询玩家物品
    sprintf(szsql,
            "select item_id,item_count from user_item where u_id = %d",
            rq->playerId);

    if(!sql.SelectMySql(szsql, 2, liststr))
    {
        // 查询失败，直接返回错误
        initbag_rs.Result = _initializebag_error;
        QByteArray packet = PacketBuilder::build(_default_protocol_initbag_rs, initbag_rs);
        emit sendToClient(clientId, packet);
        return;
    }

    // 辅助函数：去除字符串两端空白字符
    auto trim = [](const std::string &s) -> std::string {
        size_t start = s.find_first_not_of(" \t\n\r");
        size_t end = s.find_last_not_of(" \t\n\r");
        return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
    };

    std::list<std::string> equipmentList;
    sprintf(szsql,
            "select "
            "u_equipment_weapon,"
            "u_equipment_head,"
            "u_equipment_body,"
            "u_equipment_legs,"
            "u_equipment_hands,"
            "u_equipment_shoes,"
            "u_equipment_shield "
            "from user_basic_information where u_id = %d",
            rq->playerId);

    if (sql.SelectMySql(szsql, MAX_EQUIPMENT_SLOT_NUM, equipmentList)) {
        for (int i = 0; i < MAX_EQUIPMENT_SLOT_NUM && !equipmentList.empty(); ++i) {
            try {
                initbag_rs.equippedItemIds[i] = std::stoi(trim(equipmentList.front()));
            } catch (...) {
                initbag_rs.equippedItemIds[i] = 0;
            }
            equipmentList.pop_front();
        }
    }

    std::list<std::string> equipmentStateRows;
    sprintf(szsql,
            "select slot_index,item_id,enhance_level,forge_level,enchant_kind,enchant_value "
            "from user_equipment_state where u_id = %d order by slot_index asc",
            rq->playerId);

    if (sql.SelectMySql(szsql, 6, equipmentStateRows)) {
        while (equipmentStateRows.size() >= 6) {
            int slotIndex = 0;
            try {
                slotIndex = std::stoi(trim(equipmentStateRows.front()));
            } catch (...) {
                slotIndex = -1;
            }
            equipmentStateRows.pop_front();

            auto readValue = [&equipmentStateRows, &trim]() -> int {
                int value = 0;
                try {
                    value = std::stoi(trim(equipmentStateRows.front()));
                } catch (...) {
                    value = 0;
                }
                equipmentStateRows.pop_front();
                return value;
            };

            const int itemId = readValue();
            const int enhanceLevel = readValue();
            const int forgeLevel = readValue();
            const int enchantKind = readValue();
            const int enchantValue = readValue();

            if (slotIndex < 0 || slotIndex >= MAX_EQUIPMENT_SLOT_NUM) {
                continue;
            }

            initbag_rs.equippedItemIds[slotIndex] = itemId;
            initbag_rs.equippedEnhanceLevels[slotIndex] = enhanceLevel;
            initbag_rs.equippedForgeLevels[slotIndex] = forgeLevel;
            initbag_rs.equippedEnchantKinds[slotIndex] = enchantKind;
            initbag_rs.equippedEnchantValues[slotIndex] = enchantValue;
        }
    }

    // list -> vector，保证每行有两列
    auto it = liststr.begin();
    while(it != liststr.end())
    {
        std::vector<std::string> row;

        row.push_back(*it++);
        if(it == liststr.end()) break;

        row.push_back(*it++);
        if(row.size() == 2)
            result.push_back(row);
    }

    // 限制背包最大容量
    int itemCount = std::min((int)result.size(), MAX_BAG_ITEM_NUM);
    initbag_rs.itemAmount = itemCount;

    // 遍历填充背包
    bool allValid = true;  // 用于记录是否所有行都解析成功
    for(int i = 0; i < itemCount; ++i)
    {
        try {
            std::string idStr = trim(result[i][0]);
            std::string countStr = trim(result[i][1]);

            initbag_rs.playerBag[i][0] = std::stoi(idStr);
            initbag_rs.playerBag[i][1] = std::stoi(countStr);
        }
        catch(const std::invalid_argument& e) {
            qWarning() << "Invalid number in DB for player" << rq->playerId
                       << "row" << i << ":"
                       << QString::fromStdString(result[i][0])
                       << QString::fromStdString(result[i][1]);
            initbag_rs.playerBag[i][0] = 0;
            initbag_rs.playerBag[i][1] = 0;
            allValid = false;
        }
        catch(const std::out_of_range& e) {
            qWarning() << "Number out of range in DB for player" << rq->playerId
                       << "row" << i;
            initbag_rs.playerBag[i][0] = 0;
            initbag_rs.playerBag[i][1] = 0;
            allValid = false;
        }
    }

    for (int i = 0;i < itemCount;i++) {
        qDebug()<<initbag_rs.playerBag[i][0]<<":"<<initbag_rs.playerBag[i][1];
    }
    // 设置最终结果状态
    initbag_rs.Result = allValid ? _initializebag_success : _initializebag_fail;
    qDebug()<<"Result:"<<initbag_rs.Result;
    // 发送给客户端
    QByteArray packet = PacketBuilder::build(_default_protocol_initbag_rs, initbag_rs);
    emit sendToClient(clientId, packet);
}

void core::Location_Request(quint64 clientId, STRU_LOCATION_RQ* rq)
{
    JaegerDebug();

    // 更新玩家位置,将玩家位置存入vector数组
    bool found = false;
    for (auto& p : players) {
        if (p.player_UserId == rq->player_UserId) {
            p.playerName = rq->player_Name;
            p.positionX = rq->x;
            p.positionY = rq->y;
            found = true;
            break;
        }
    }
    // 如果没有记录，则新插入
    if (!found) {
        const CombatBalance::PlayerStats baseline = CombatBalance::playerStats(1);
        players.emplace_back(rq->player_UserId,
                             QString::fromUtf8(rq->player_Name),
                             1,
                             0,
                             0,
                             baseline.maxHealth,
                             baseline.maxMana,
                             baseline.attack,
                             baseline.magicAttack,
                             baseline.independentAttack,
                             baseline.defence,
                             baseline.magicDefence,
                             baseline.strength,
                             baseline.intelligence,
                             baseline.vitality,
                             baseline.spirit,
                             baseline.critRate,
                             baseline.magicCritRate,
                             baseline.critDamage,
                             baseline.attackSpeed,
                             baseline.moveSpeed,
                             baseline.castSpeed,
                             baseline.attackRange,
                             rq->x,
                             rq->y);
    }

    // 构造位置同步数据包
    STRU_LOCATION_RS sls;
    sls.playerCount = std::min((int)players.size(), 50);

    for (int i = 0; i < sls.playerCount; ++i) {
        strcpy(sls.players[i].player_Name, players[i].playerName.toUtf8().constData());
        sls.players[i].player_UserId = players[i].player_UserId;
        sls.players[i].x = players[i].positionX;
        sls.players[i].y = players[i].positionY;
    }

    // 广播给所有客户端
    const auto& allClients = m_pTCPNet->getAllClientIds();
    QByteArray packet_loc = PacketBuilder::build(_default_protocol_location_rs, sls);
    for (quint64 cid : allClients) {
        if (cid != clientId) {
            // 如果KCP已建立，优先使用KCP广播位置
            if (m_pKcpNet && m_pKcpNet->isKcpConnected(cid)) {
                emit sendToClientKcp(cid, packet_loc);
            } else {
                emit sendToClient(cid, packet_loc);
            }
        }
    }
}
void core::PlayerList_Request(quint64 clientId, STRU_PLAYERLIST_RQ* rq){

}

void core::Save_Request(quint64 clientId, STRU_SAVE_RQ* rq){
    JaegerDebug();

    ensureEquipmentColumns();
    ensureEquipmentStateTable();
    ensureProgressStateTable();
    STRU_SAVE_RS sss;
    CMySql sql;
    sss.Save_Result = _save_fail_;
    char szsql[MAXSIZE * 3] = {0};
    const int normalizedLevel = CombatBalance::clampLevel(rq->level);
    const CombatBalance::PlayerStats baseline = CombatBalance::playerStats(normalizedLevel);
    const int normalizedHealth = qBound(1, rq->health, baseline.maxHealth);
    const int normalizedMana = qBound(0, rq->mana, baseline.maxMana);
    const int normalizedAttackRange = qBound(20, rq->attackRange, 160);
    const long long normalizedExperience = qMax(0LL, rq->experience);
    snprintf(szsql,
            sizeof(szsql),
            "UPDATE user_basic_information SET "
            "u_health = '%d', "
            "u_mana = '%d', "
            "u_attackpower = '%d', "
            "u_magicattack = '%d', "
            "u_independentattack = '%d', "
            "u_attackrange = '%d', "
            "u_experience = '%lld', "
            "u_level = '%d', "
            "u_defence = '%d', "
            "u_magicdefence = '%d', "
            "u_strength = '%d', "
            "u_intelligence = '%d', "
            "u_vitality = '%d', "
            "u_spirit = '%d', "
            "u_critrate = '%.3f', "
            "u_magiccritrate = '%.3f', "
            "u_critdamage = '%.3f', "
            "u_attackspeed = '%.3f', "
            "u_movespeed = '%.3f', "
            "u_castspeed = '%.3f', "
            "u_position_x = '%.2f', "
            "u_position_y = '%.2f', "
            "u_equipment_weapon = '%d', "
            "u_equipment_head = '%d', "
            "u_equipment_body = '%d', "
            "u_equipment_legs = '%d', "
            "u_equipment_hands = '%d', "
            "u_equipment_shoes = '%d', "
            "u_equipment_shield = '%d' "
            "WHERE u_id = '%d';",
            normalizedHealth, normalizedMana,
            baseline.attack, baseline.magicAttack, baseline.independentAttack, normalizedAttackRange, normalizedExperience,
            normalizedLevel, baseline.defence, baseline.magicDefence,
            baseline.strength, baseline.intelligence, baseline.vitality, baseline.spirit,
            baseline.critRate, baseline.magicCritRate, baseline.critDamage,
            baseline.attackSpeed, baseline.moveSpeed, baseline.castSpeed,
            rq->x, rq->y,
            rq->equippedItemIds[0], rq->equippedItemIds[1], rq->equippedItemIds[2],
            rq->equippedItemIds[3], rq->equippedItemIds[4], rq->equippedItemIds[5],
            rq->equippedItemIds[6],
            rq->player_UserId
            );
    bool savedOk = sql.UpdateMySql(szsql);

    if (savedOk) {
        for (int slotIndex = 0; slotIndex < MAX_EQUIPMENT_SLOT_NUM; ++slotIndex) {
            sprintf(szsql,
                    "INSERT INTO user_equipment_state "
                    "(u_id, slot_index, item_id, enhance_level, forge_level, enchant_kind, enchant_value) "
                    "VALUES ('%d', '%d', '%d', '%d', '%d', '%d', '%d') "
                    "ON DUPLICATE KEY UPDATE "
                    "item_id = VALUES(item_id), "
                    "enhance_level = VALUES(enhance_level), "
                    "forge_level = VALUES(forge_level), "
                    "enchant_kind = VALUES(enchant_kind), "
                    "enchant_value = VALUES(enchant_value);",
                    rq->player_UserId,
                    slotIndex,
                    rq->equippedItemIds[slotIndex],
                    rq->equippedEnhanceLevels[slotIndex],
                    rq->equippedForgeLevels[slotIndex],
                    rq->equippedEnchantKinds[slotIndex],
                    rq->equippedEnchantValues[slotIndex]);
            if (!sql.UpdateMySql(szsql)) {
                savedOk = false;
                break;
            }
        }
    }

    if (savedOk) {
        sprintf(szsql,
                "DELETE FROM user_item WHERE u_id = '%d';",
                rq->player_UserId);
        if (!sql.UpdateMySql(szsql)) {
            savedOk = false;
        }
    }

    if (savedOk) {
        const int safeBagAmount = qBound(0, rq->bagItemAmount, MAX_BAG_ITEM_NUM);
        for (int i = 0; i < safeBagAmount; ++i) {
            const int itemId = rq->bagItems[i][0];
            const int itemCount = rq->bagItems[i][1];
            if (itemId <= 0 || itemCount <= 0) {
                continue;
            }

            sprintf(szsql,
                    "INSERT INTO user_item (u_id, item_id, item_count) "
                    "VALUES ('%d', '%d', '%d');",
                    rq->player_UserId,
                    itemId,
                    itemCount);
            if (!sql.UpdateMySql(szsql)) {
                savedOk = false;
                break;
            }
        }
    }

    if (savedOk) {
        QString mapId = QString::fromUtf8(rq->mapId).trimmed();
        if (mapId.isEmpty()) {
            mapId = QStringLiteral("BornWorld");
        }
        mapId.replace('\'', "''");
        const QByteArray safeMapUtf8 = mapId.toUtf8();

        sprintf(szsql,
                "INSERT INTO user_progress_state "
                "(u_id, map_id, quest_step, pos_x, pos_y) "
                "VALUES ('%d', '%s', '%d', '%.2f', '%.2f') "
                "ON DUPLICATE KEY UPDATE "
                "map_id = VALUES(map_id), "
                "quest_step = VALUES(quest_step), "
                "pos_x = VALUES(pos_x), "
                "pos_y = VALUES(pos_y);",
                rq->player_UserId,
                safeMapUtf8.constData(),
                rq->questStep,
                rq->x,
                rq->y);

        if (!sql.UpdateMySql(szsql)) {
            savedOk = false;
        }
    }

    if(savedOk){
        sss.Save_Result = _save_success_;
    }else{
        sss.Save_Result = _save_error_;
    }
    QByteArray packet = PacketBuilder::build(_default_protocol_save_rs, sss);
    emit sendToClient(clientId, packet);
}

void core::Dazuo_Request(quint64 clientId, STRU_DAZUO_RQ* rq)
{
    JaegerDebug();
    STRU_DAZUO_RS sds;
    int tempid = rq->player_UserId;
    sds.dazuoConfirm = false;

    if (rq->isDazuo)
    {
        CMySql sql;
        char szsql[MAXSIZE] = {0};
        std::list<std::string> liststr;
        sprintf(szsql, "SELECT u_level, u_experience FROM user_basic_information WHERE u_id = '%d';", tempid);
        sql.SelectMySql(szsql, 2, liststr);

        if (liststr.size() >= 2)
        {
            std::string userLevel = liststr.front(); liststr.pop_front();
            std::string userExp = liststr.front(); liststr.pop_front();

            const int parsedLevel = CombatBalance::clampLevel(std::stoi(userLevel));
            const long long parsedExp = qMax(0LL, std::stoll(userExp));
            const CombatBalance::PlayerStats baseline = CombatBalance::playerStats(parsedLevel);
            auto playerInfo = std::make_shared<Player_Information>(
                tempid,
                "",
                parsedLevel,
                parsedExp,
                parsedExp,
                baseline.maxHealth,
                baseline.maxMana,
                baseline.attack,
                baseline.magicAttack,
                baseline.independentAttack,
                baseline.defence,
                baseline.magicDefence,
                baseline.strength,
                baseline.intelligence,
                baseline.vitality,
                baseline.spirit,
                baseline.critRate,
                baseline.magicCritRate,
                baseline.critDamage,
                baseline.attackSpeed,
                baseline.moveSpeed,
                baseline.castSpeed,
                baseline.attackRange,
                0.0f,
                0.0f);
            m_mapPlayerInfo[tempid] = playerInfo;

            if (m_mapDazuoTimer.count(tempid) && m_mapDazuoTimer[tempid])
            {
                m_mapDazuoTimer[tempid]->stop();
                delete m_mapDazuoTimer[tempid];
                m_mapDazuoTimer[tempid] = nullptr;
            }

            QTimer* timer = new QTimer(this);
            quint64 cid = clientId;
            connect(timer, &QTimer::timeout, this, [this, tempid, cid]() {
                auto it = m_mapPlayerInfo.find(tempid);
                if (it == m_mapPlayerInfo.end()) return;
                auto info = it->second;
                long long gainedExp = info->level * 2;
                info->exp += gainedExp;
                qDebug() << "玩家ID：" << tempid << ", 打坐中... 获得经验：" << gainedExp << " 当前总经验：" << info->exp;
                bool leveled = LevelUp(info->level, info->exp, tempid);
                if (leveled)
                {
                    qDebug() << "玩家升级, 当前等级：" << info->level;
                    STRU_LEVELUP_RS sls;
                    sls.MaxExp = getEXP(info->level);
                    sls.userlevel = info->level;
                    QByteArray pkt = PacketBuilder::build(_default_protocol_levelup_rs, sls);
                    emit sendToClient(cid, pkt);
                }
            });
            timer->start(2000);
            m_mapDazuoTimer[tempid] = timer;

            sds.exp = playerInfo->exp;
            sds.userlevel = playerInfo->level;
            sds.dazuoConfirm = true;
            sds.MaxExp = getEXP(playerInfo->level);
            QByteArray packet = PacketBuilder::build(_default_protocol_dazuo_rs, sds);
            emit sendToClient(clientId, packet);
        }
    }
    else
    {
        if (m_mapDazuoTimer.count(tempid) && m_mapDazuoTimer[tempid])
        {
            m_mapDazuoTimer[tempid]->stop();
            delete m_mapDazuoTimer[tempid];
            m_mapDazuoTimer.erase(tempid);
            qDebug() << "玩家ID：" << tempid << ", 打坐结束";
        }

        auto infoIt = m_mapPlayerInfo.find(tempid);
        if (infoIt != m_mapPlayerInfo.end())
        {
            auto info = infoIt->second;
            CMySql sql;
            char szsql[MAXSIZE] = {0};
            sprintf(szsql,
                    "UPDATE user_basic_information SET "
                    "u_experience = '%lld', "
                    "u_level = '%d' "
                    "WHERE u_id = '%d';",
                    info->exp, info->level, tempid);
            sql.UpdateMySql(szsql);

            sds.exp = info->exp;
            sds.userlevel = info->level;
            sds.dazuoConfirm = false;
            sds.addedExp = info->exp - info->startExp;
            qDebug() << "玩家ID:" << tempid << "打坐获得经验:" << sds.addedExp << "总经验:" << sds.exp;
            QByteArray packet = PacketBuilder::build(_default_protocol_dazuo_rs, sds);
            emit sendToClient(clientId, packet);

            m_mapPlayerInfo.erase(tempid);
        }
    }
}

bool core::LevelUp(int& lvl,long long& userExp,int player_id)
{
    JaegerDebug();
    CMySql sql;
    bool leveledUp = false;
    int newLevel = CombatBalance::clampLevel(lvl);
    long long remainedExp = qMax(0LL, userExp);

    while(remainedExp >= getEXP(newLevel) && newLevel < 100){
        remainedExp -= getEXP(newLevel);
        ++newLevel;
        leveledUp = true;
    }

    lvl = newLevel;
    userExp = remainedExp;

    if(leveledUp){
        const CombatBalance::PlayerStats baseline = CombatBalance::playerStats(lvl);
        char szsql[MAXSIZE * 3] = {0};
        snprintf(szsql,
                sizeof(szsql),
                "UPDATE user_basic_information SET "
                "u_health = '%d', "
                "u_mana = '%d', "
                "u_attackpower = '%d', "
                "u_magicattack = '%d', "
                "u_independentattack = '%d', "
                "u_experience = '%lld', "
                "u_level = '%d', "
                "u_defence = '%d', "
                "u_magicdefence = '%d', "
                "u_strength = '%d', "
                "u_intelligence = '%d', "
                "u_vitality = '%d', "
                "u_spirit = '%d', "
                "u_critrate = '%.3f', "
                "u_magiccritrate = '%.3f', "
                "u_critdamage = '%.3f', "
                "u_attackspeed = '%.3f', "
                "u_movespeed = '%.3f', "
                "u_castspeed = '%.3f', "
                "u_attackrange = '%d' "
                "WHERE u_id = '%d';",
                baseline.maxHealth,
                baseline.maxMana,
                baseline.attack,
                baseline.magicAttack,
                baseline.independentAttack,
                userExp,
                lvl,
                baseline.defence,
                baseline.magicDefence,
                baseline.strength,
                baseline.intelligence,
                baseline.vitality,
                baseline.spirit,
                baseline.critRate,
                baseline.magicCritRate,
                baseline.critDamage,
                baseline.attackSpeed,
                baseline.moveSpeed,
                baseline.castSpeed,
                baseline.attackRange,
                player_id
                );
        if (m_mapPlayerInfo.count(player_id) && m_mapPlayerInfo[player_id]) {
            m_mapPlayerInfo[player_id]->exp = userExp;
            m_mapPlayerInfo[player_id]->level = lvl;
            m_mapPlayerInfo[player_id]->health = baseline.maxHealth;
            m_mapPlayerInfo[player_id]->mana = baseline.maxMana;
            m_mapPlayerInfo[player_id]->attackPower = baseline.attack;
            m_mapPlayerInfo[player_id]->magicAttack = baseline.magicAttack;
            m_mapPlayerInfo[player_id]->independentAttack = baseline.independentAttack;
            m_mapPlayerInfo[player_id]->defense = baseline.defence;
            m_mapPlayerInfo[player_id]->magicDefense = baseline.magicDefence;
            m_mapPlayerInfo[player_id]->strength = baseline.strength;
            m_mapPlayerInfo[player_id]->intelligence = baseline.intelligence;
            m_mapPlayerInfo[player_id]->vitality = baseline.vitality;
            m_mapPlayerInfo[player_id]->spirit = baseline.spirit;
            m_mapPlayerInfo[player_id]->critRate = baseline.critRate;
            m_mapPlayerInfo[player_id]->magicCritRate = baseline.magicCritRate;
            m_mapPlayerInfo[player_id]->critDamage = baseline.critDamage;
            m_mapPlayerInfo[player_id]->attackSpeed = baseline.attackSpeed;
            m_mapPlayerInfo[player_id]->moveSpeed = baseline.moveSpeed;
            m_mapPlayerInfo[player_id]->castSpeed = baseline.castSpeed;
            m_mapPlayerInfo[player_id]->attackRange = baseline.attackRange;
        }
        return sql.UpdateMySql(szsql);
    }
    return false;
}

void core::HandleAttack(quint64 clientId, STRU_ATTACK_RQ* rq) {
    JaegerDebug();

    // 获取目标血量
    int targetHealth = getHealth(rq->targetId);
    if (targetHealth <= 0) {
        qWarning() << "目标玩家" << rq->targetId << "已经死亡或不存在";
        return;
    }

    // 简化伤害计算：直接使用客户端传来的伤害值
    int finalDamage = std::max(1, (int)rq->damage);
    int newHealth = std::max(0, targetHealth - finalDamage);

    if (!updateHealthInDB(rq->targetId, newHealth)) {
        qWarning() << "更新目标玩家" << rq->targetId << "的血量失败";
        return;
    }

    STRU_ATTACK_RS rs;
    rs.targetId = rq->targetId;
    rs.currentHealth = newHealth;
    rs.isDead = (newHealth <= 0);

    // 广播给所有客户端
    QByteArray packet = PacketBuilder::build(_default_protocol_attack_rs, rs);
    const auto& allClients = m_pTCPNet->getAllClientIds();
    for (quint64 cid : allClients) {
        if (m_pKcpNet && m_pKcpNet->isKcpConnected(cid)) {
            emit sendToClientKcp(cid, packet);
        } else {
            emit sendToClient(cid, packet);
        }
    }

    // 如果死亡，10秒后复活
    if (rs.isDead) {
        int deadId = rs.targetId;
        QTimer::singleShot(10000, this, [this, deadId]() {
            int reviveHealth = 100;
            CMySql sql;
            char szsql[MAXSIZE] = {0};
            std::list<std::string> levelResult;
            snprintf(szsql,
                     sizeof(szsql),
                     "SELECT u_level FROM user_basic_information WHERE u_id = '%d' LIMIT 1;",
                     deadId);
            if (sql.SelectMySql(szsql, 1, levelResult) && !levelResult.empty()) {
                try {
                    reviveHealth = static_cast<int>(getHP(std::stoi(levelResult.front())));
                } catch (...) {
                    reviveHealth = 100;
                }
            }

            updateHealthInDB(deadId, reviveHealth);
            STRU_REVIVE_RS srr;
            srr.playerId = deadId;
            srr.newhealth = reviveHealth;
            QByteArray revivePacket = PacketBuilder::build(_default_protocol_revive_rs, srr);
            const auto& clients = m_pTCPNet->getAllClientIds();
            for (quint64 cid : clients) {
                emit sendToClient(cid, revivePacket);
            }
        });
    }
}

int core::calculateDamage(int attackedId, int targetId, int clientDamage)
{
    // // 获取攻击者和目标的属性
    // PlayerAttributes attackerAttributes = getPlayerAttributesFromDB(attackedId);
    // PlayerAttributes targetAttributes = getPlayerAttributesFromDB(targetId);

    // // 如果任何一个玩家的属性失败，返回0伤害
    // if (attackerAttributes.health <= 0 || targetAttributes.health <= 0) {
    //     return 0;
    // }

    // // 基础伤害 = 客户端计算的伤害 + 攻击者的攻击力
    // int baseDamage = clientDamage + attackerAttributes.attackPower;

    // // 计算目标的防御值，减少伤害
    // int reducedDamage = baseDamage - targetAttributes.defense;
    // if (reducedDamage < 0) {
    //     reducedDamage = 0;  // 防止伤害为负数
    // }

    // // 判断是否暴击，暴击率计算
    // float critChance = attackerAttributes.critRate;
    // bool isCrit = ((rand() % 100) < (critChance * 100));  // 暴击发生的概率（比如暴击率是20%，则概率是20%）
    // if (isCrit) {
    //     // 暴击伤害，假设暴击伤害为2倍
    //     reducedDamage = reducedDamage * attackerAttributes.critDamage;
    //     qDebug() << "暴击！伤害倍增！";
    // }

    // // 最终伤害 = 最大0和计算的伤害
    // return std::max(0, reducedDamage);
    Q_UNUSED(attackedId);
    Q_UNUSED(targetId);
    return qMax(0, clientDamage);
}

// Player_Information core::getPlayerAttributesFromDB(int userId) {
//     // char szsql[MAXSIZE] = {0};
//     // list<string> liststr;
//     // PlayerAttributes attributes;

//     // // 查询所有需要的字段
//     // sprintf(szsql,
//     //         "SELECT u_health, u_attackpower, u_defence, u_critrate, u_critdamage, "
//     //         "u_attackrange, u_experience, u_level, u_position_x, u_position_y "
//     //         "FROM user_basic_information WHERE u_id = %d;", userId);

//     // sql->SelectMySql(szsql, 10, liststr);

//     // if (liststr.size() < 10) {
//     //     qWarning() << "获取玩家" << userId << "的属性失败，数据不完整";
//     //     return attributes; // 返回默认值
//     // }

//     // // 解析数据
//     // attributes.health = std::stoi(liststr.front()); liststr.pop_front();
//     // attributes.attackPower = std::stoi(liststr.front()); liststr.pop_front();
//     // attributes.defense = std::stoi(liststr.front()); liststr.pop_front();
//     // attributes.critRate = std::stof(liststr.front()); liststr.pop_front();
//     // attributes.critDamage = std::stof(liststr.front()); liststr.pop_front();
//     // attributes.attackRange = std::stof(liststr.front()); liststr.pop_front();
//     // attributes.experience = std::stoll(liststr.front()); liststr.pop_front();
//     // attributes.level = std::stoi(liststr.front()); liststr.pop_front();
//     // attributes.positionX = std::stof(liststr.front()); liststr.pop_front();
//     // attributes.positionY = std::stof(liststr.front()); liststr.pop_front();
//     // // attributes.state = static_cast<PlayerState>(std::stof(liststr.front()));
//     // // liststr.pop_front();
//     // //attributes.ghostEndTime = std::stoi(liststr.front()); liststr.pop_front();

//     // return attributes;
// }

int core::getHealth(int userId) {
    CMySql sql;
    char szsql[MAXSIZE] = {0};
    std::list<std::string> liststr;
    sprintf(szsql, "SELECT u_health FROM user_basic_information WHERE u_id = %d;", userId);
    if (!sql.SelectMySql(szsql, 1, liststr) || liststr.empty()) {
        qWarning() << "获取玩家" << userId << "血量失败";
        return -1;
    }
    return std::stoi(liststr.front());
}

bool core:: updateHealthInDB(int userId, int newHealth) {

    CMySql sql;
    char szsql[MAXSIZE] = {0};
    sprintf(szsql, "UPDATE user_basic_information SET u_health = %d WHERE u_id = %d;",
            newHealth, userId);

    if (!sql.UpdateMySql(szsql)) {
        qCritical() << "更新玩家" << userId << "血量失败";
        return false;
    }
    return true;
}





























