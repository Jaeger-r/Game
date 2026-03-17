#include "MySqlConnPool.h"
#include <stdexcept>

/* ============ 单例 ============ */

MySqlConnPool& MySqlConnPool::Instance() {
    static MySqlConnPool pool;
    return pool;
}

MYSQL* MySqlConnPool::createConnection() {
    MYSQL* conn = mysql_init(nullptr);
    if (!conn)
        return nullptr;

    if (!mysql_real_connect(conn,
                            m_host.c_str(),
                            m_user.c_str(),
                            m_pass.c_str(),
                            m_db.c_str(),
                            0, nullptr, 0))
    {
        mysql_close(conn);
        return nullptr;
    }

    if (mysql_set_character_set(conn, "utf8") != 0) {
        mysql_close(conn);
        return nullptr;
    }

    return conn;
}
/* ============ 初始化 ============ */

void MySqlConnPool::init(const std::string& host,
                         const std::string& user,
                         const std::string& pass,
                         const std::string& db,
                         int poolSize)
{
    m_host = host;
    m_user = user;
    m_pass = pass;
    m_db   = db;
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_inited)
        return;

    if (poolSize <= 0)
        throw std::runtime_error("poolSize must > 0");

    try {
        for (int i = 0; i < poolSize; ++i) {
            MYSQL* conn = createConnection();
            if (!conn)
                throw std::runtime_error("createConnection failed");

            m_idle.push_back(conn);
            ++m_total;
        }
    }
    catch (...) {
        for (auto* c : m_idle)
            mysql_close(c);
        m_idle.clear();
        throw;
    }

    m_inited = true;
}

/* ============ 获取连接 ============ */

MYSQL* MySqlConnPool::acquire() {
    // MySQL 要求每个线程初始化
    mysql_thread_init();

    std::unique_lock<std::mutex> lock(m_mutex);

    m_cv.wait(lock, [this] {
        return m_shutdown || !m_idle.empty();
    });

    if (m_shutdown)
        return nullptr;

    MYSQL* conn = m_idle.back();
    m_idle.pop_back();
    ++m_inUse;

    // 检测连接是否存活
    if (mysql_ping(conn) != 0) {

        mysql_close(conn);
        --m_total;

        // 尝试重建
        MYSQL* newConn = createConnection();
        if (!newConn) {
            --m_inUse;
            return nullptr;
        }

        ++m_total;
        return newConn;
    }

    return conn;
}

/* ============ 归还连接 ============ */

void MySqlConnPool::release(MYSQL* conn) {
    if (!conn) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_shutdown) {
        mysql_close(conn);
        return;
    }
    if (mysql_ping(conn) != 0) {
        mysql_close(conn);
        --m_total;
        return;
    }

    --m_inUse;
    m_idle.push_back(conn);
    m_cv.notify_one();
}

/* ============ 析构 ============ */

MySqlConnPool::~MySqlConnPool() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_shutdown = true;

    // 等所有连接归还
    m_cv.wait(lock, [this] {
        return m_inUse == 0;
    });

    for (auto* c : m_idle)
        mysql_close(c);

    m_idle.clear();
}

/* ============ RAII Guard ============ */

MySqlConnGuard::MySqlConnGuard() {
    m_conn = MySqlConnPool::Instance().acquire();
}

MySqlConnGuard::~MySqlConnGuard() {
    if (m_conn)
        MySqlConnPool::Instance().release(m_conn);

    mysql_thread_end();   // 线程结束清理
}
