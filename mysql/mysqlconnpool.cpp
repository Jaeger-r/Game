#include "mysqlconnpool.h"
#include <QDebug>
#include <stdexcept>

/* ============ 单例 ============ */

/**
 * @brief 处理Instance相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
MySqlConnPool& MySqlConnPool::Instance() {
    static MySqlConnPool pool;
    return pool;
}

/**
 * @brief 创建createConnection相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
MYSQL* MySqlConnPool::createConnection() {
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {
        qWarning() << "mysql_init returned nullptr while creating pooled connection.";
        return nullptr;
    }

    if (!mysql_real_connect(conn,
                            m_host.c_str(),
                            m_user.c_str(),
                            m_pass.c_str(),
                            m_db.c_str(),
                            m_port, nullptr, 0))
    {
        qWarning() << "mysql_real_connect failed for"
                   << QString::fromStdString(m_host)
                   << QString::fromStdString(m_db)
                   << ":" << mysql_error(conn);
        mysql_close(conn);
        return nullptr;
    }

    const char* charsetCandidates[] = {"utf8mb4", "utf8mb3", "utf8"};
    bool charsetOk = false;
    for (const char* charset : charsetCandidates) {
        if (mysql_set_character_set(conn, charset) == 0) {
            charsetOk = true;
            break;
        }
    }
    if (!charsetOk) {
        qWarning() << "mysql_set_character_set failed for all candidates on"
                   << QString::fromStdString(m_db)
                   << ":" << mysql_error(conn);
        mysql_close(conn);
        return nullptr;
    }

    return conn;
}
/* ============ 初始化 ============ */

/**
 * @brief 初始化init相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void MySqlConnPool::init(const std::string& host,
                         const std::string& user,
                         const std::string& pass,
                         const std::string& db,
                         unsigned int port,
                         int poolSize)
{
    m_host = host;
    m_user = user;
    m_pass = pass;
    m_db   = db;
    m_port = port;
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_inited)
        return;

    if (poolSize <= 0)
        throw std::runtime_error("poolSize must > 0");

    try {
        for (int i = 0; i < poolSize; ++i) {
            MYSQL* conn = createConnection();
            if (!conn) {
                qWarning() << "Failed to create pooled MySQL connection at index" << i;
                throw std::runtime_error("createConnection failed");
            }

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

/**
 * @brief 处理acquire相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
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

/**
 * @brief 处理release相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
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

/**
 * @brief 析构MySqlConnPool对象并释放相关资源
 * @author Jaeger
 * @date 2025.3.28
 */
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

/**
 * @brief 构造MySqlConnGuard对象并完成基础初始化
 * @author Jaeger
 * @date 2025.3.28
 */
MySqlConnGuard::MySqlConnGuard() {
    m_conn = MySqlConnPool::Instance().acquire();
}

/**
 * @brief 析构MySqlConnGuard对象并释放相关资源
 * @author Jaeger
 * @date 2025.3.28
 */
MySqlConnGuard::~MySqlConnGuard() {
    if (m_conn)
        MySqlConnPool::Instance().release(m_conn);

    mysql_thread_end();   // 线程结束清理
}
