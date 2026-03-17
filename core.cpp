#include "core.h"
#include <QDateTime>
#include <QDebug>
#include <QTextStream>

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
            "insert into user_basic_information(u_id,u_name) values (%d,'%s');",
            user_id,
            rq->player_Name
            );
    if (!mysql.UpdateMySql(sql))
        return REG_INSERT_FAIL;
    result.clear();
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

    CMySql sql;
    STRU_INITIALIZE_RS initialize_rs;
    initialize_rs.Initialize_Result = _initialize_fail;

    char szsql[MAXSIZE] = {0};
    list<string>liststr;
    sprintf(szsql,"select u_name,u_health,u_attackpower,u_attackrange,u_experience,u_level,u_defence,u_critrate,u_critdamage,u_position_x,u_position_y from user_basic_information where u_id = '%d';",rq->player_UserId);
    if (sql.SelectMySql(szsql,11,liststr))
        initialize_rs.Initialize_Result = _initialize_error;
    if (!liststr.empty()) {
        //1
        string strUserName = liststr.front();
        snprintf(initialize_rs.player_Name,
                 sizeof(initialize_rs.player_Name),
                 "%s",
                 strUserName.c_str());
        liststr.pop_front();
        //2
        string strUserHealth = liststr.front();
        initialize_rs.health = std::stoi(strUserHealth);
        liststr.pop_front();
        //3
        string strUserAttackPower = liststr.front();
        initialize_rs.attackPower = std::stof(strUserAttackPower);
        liststr.pop_front();
        //4
        string strUserAttackRange = liststr.front();
        initialize_rs.attackRange = std::stof(strUserAttackRange);
        liststr.pop_front();
        //5
        string strUserExperience = liststr.front();
        initialize_rs.experience = std::stoll(strUserExperience);
        liststr.pop_front();
        //6
        string strUserLevel = liststr.front();
        initialize_rs.level = std::stoi(strUserLevel);
        liststr.pop_front();
        //7
        string strUserDefence = liststr.front();
        initialize_rs.defence = std::stoi(strUserDefence);
        liststr.pop_front();
        //8
        string strUserCritRate = liststr.front();
        initialize_rs.critical_rate = std::stof(strUserCritRate);
        liststr.pop_front();
        //9
        string strUserCritDamage = liststr.front();
        initialize_rs.critical_damage = std::stof(strUserCritDamage);
        liststr.pop_front();
        //10
        string strUserPositionX = liststr.front();
        initialize_rs.x = std::stof(strUserPositionX);
        liststr.pop_front();
        //11
        string strUserPositionY = liststr.front();
        initialize_rs.y = std::stof(strUserPositionY);
        liststr.pop_front();
        //12
        initialize_rs.Initialize_Result = _initialize_success;
        //13
        initialize_rs.player_UserId = rq->player_UserId;
        qDebug()<<"Init Success";
        QByteArray packet = PacketBuilder::build(_default_protocol_initialize_rs,initialize_rs);
        emit sendToClient(clientId, packet);
        //m_pTCPNet->sendData(sock,(char*)&initialize_rs,sizeof(initialize_rs));
    }else{
        initialize_rs.Initialize_Result = _initialize_error;
        QByteArray packet = PacketBuilder::build(_default_protocol_initialize_rs,initialize_rs);
        emit sendToClient(clientId, packet);
        //m_pTCPNet->sendData(sock,(char*)&initialize_rs,sizeof(initialize_rs));
    }
}

void core::InitializeBag_Request(quint64 clientId, STRU_INITBAG_RQ* rq)
{
    JaegerDebug();

    CMySql sql;
    STRU_INITBAG_RS initbag_rs;
    char szsql[MAXSIZE] = {0};

    std::vector<std::vector<std::string>> result;
    std::list<std::string> liststr;

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
        players.emplace_back(rq->player_UserId, QString::fromUtf8(rq->player_Name),
                             1, 0, 0, 100, 10, 5, 0.0f, 1.0f, 0.0f,
                             rq->x, rq->y);
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
            emit sendToClient(cid, packet_loc);
        }
    }
}
void core::PlayerList_Request(quint64 clientId, STRU_PLAYERLIST_RQ* rq){

}

void core::Save_Request(quint64 clientId, STRU_SAVE_RQ* rq){
    JaegerDebug();

    STRU_SAVE_RS sss;
    CMySql sql;
    sss.Save_Result = _save_fail_;
    char szsql[MAXSIZE] = {0};
    list<string>liststr;
    sprintf(szsql,
            "UPDATE user_basic_information SET "
            "u_health = '%d', "
            "u_attackpower = '%d', "
            "u_attackrange = '%d', "
            "u_experience = '%lld', "
            "u_level = '%d', "
            "u_defence = '%d', "
            "u_critrate = '%.2f', "
            "u_critdamage = '%.2f' "
            "WHERE u_id = '%d';",
            rq->health, rq->attackPower, rq->attackRange, rq->experience,
            rq->level, rq->defence, rq->critical_rate, rq->critical_damage, rq->player_UserId
            );
    if(sql.UpdateMySql(szsql)){
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

            auto playerInfo = std::make_shared<Player_Information>(
                tempid, "", std::stoi(userLevel), std::stoll(userExp), std::stoll(userExp),
                0, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
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
    while(userExp>=getEXP(lvl)){
        userExp -= getEXP(lvl);
        ++lvl;
        leveledUp = true;
    }
    if(leveledUp){
        char szsql[MAXSIZE] = {0};
        sprintf(szsql,
                "UPDATE user_basic_information SET "
                "u_experience = '%lld', "
                "u_level = '%d' "
                "WHERE u_id = '%d';",
                0,lvl,player_id
                );
        m_mapPlayerInfo[player_id]->exp = 0;
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
        emit sendToClient(cid, packet);
    }

    // 如果死亡，10秒后复活
    if (rs.isDead) {
        int deadId = rs.targetId;
        QTimer::singleShot(10000, this, [this, deadId]() {
            updateHealthInDB(deadId, 100);
            STRU_REVIVE_RS srr;
            srr.playerId = deadId;
            srr.newhealth = 100;
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





























