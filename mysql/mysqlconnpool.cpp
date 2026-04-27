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

PGconn* MySqlConnPool::createConnection() {
    const std::string portText = std::to_string(m_port);
    PGconn* conn = PQsetdbLogin(m_host.c_str(),
                                portText.c_str(),
                                nullptr,
                                nullptr,
                                m_db.c_str(),
                                m_user.c_str(),
                                m_pass.c_str());
    if (!conn) {
        qWarning() << "PQsetdbLogin returned nullptr while creating pooled connection.";
        return nullptr;
    }

    if (PQstatus(conn) != CONNECTION_OK) {
        qWarning() << "PostgreSQL connect failed for"
                   << QString::fromStdString(m_host)
                   << QString::fromStdString(m_db)
                   << ":" << QString::fromLocal8Bit(PQerrorMessage(conn)).trimmed();
        PQfinish(conn);
        return nullptr;
    }

    if (PQsetClientEncoding(conn, "UTF8") != 0) {
        qWarning() << "Failed to set PostgreSQL client encoding to UTF8 for"
                   << QString::fromStdString(m_db)
                   << ":" << QString::fromLocal8Bit(PQerrorMessage(conn)).trimmed();
        PQfinish(conn);
        return nullptr;
    }

    PGresult* result = PQexec(conn, "SET application_name = 'Jaeger GameServer';");
    if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
        qWarning() << "Failed to set PostgreSQL application_name:"
                   << QString::fromLocal8Bit(PQerrorMessage(conn)).trimmed();
        if (result) {
            PQclear(result);
        }
        PQfinish(conn);
        return nullptr;
    }
    PQclear(result);

    return conn;
}

bool MySqlConnPool::isConnectionHealthy(PGconn* conn) const
{
    if (!conn || PQstatus(conn) != CONNECTION_OK) {
        return false;
    }

    PGresult* result = PQexec(conn, "SELECT 1;");
    if (!result) {
        return false;
    }

    const bool ok = PQresultStatus(result) == PGRES_TUPLES_OK
        && PQntuples(result) == 1
        && PQnfields(result) == 1;
    PQclear(result);
    return ok;
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
    m_targetPoolSize = poolSize;
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_inited)
        return;

    if (poolSize <= 0)
        throw std::runtime_error("poolSize must > 0");

    try {
        for (int i = 0; i < poolSize; ++i) {
            PGconn* conn = createConnection();
            if (!conn) {
                qWarning() << "Failed to create pooled PostgreSQL connection at index" << i;
                throw std::runtime_error("createConnection failed");
            }

            m_idle.push_back(conn);
            ++m_total;
        }
    }
    catch (...) {
        for (auto* c : m_idle)
            PQfinish(c);
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
PGconn* MySqlConnPool::acquire() {
    std::unique_lock<std::mutex> lock(m_mutex);

    m_cv.wait(lock, [this] {
        return m_shutdown || !m_idle.empty() || m_total < m_targetPoolSize;
    });

    if (m_shutdown)
        return nullptr;

    if (m_idle.empty() && m_total < m_targetPoolSize) {
        ++m_inUse;
        ++m_total;
        lock.unlock();

        PGconn* newConn = createConnection();

        lock.lock();
        if (!newConn) {
            --m_inUse;
            --m_total;
            m_cv.notify_one();
            return nullptr;
        }
        return newConn;
    }

    PGconn* conn = m_idle.back();
    m_idle.pop_back();
    ++m_inUse;

    if (!isConnectionHealthy(conn)) {

        PQfinish(conn);
        --m_total;

        PGconn* newConn = createConnection();
        if (!newConn) {
            --m_inUse;
            m_cv.notify_one();
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
void MySqlConnPool::release(PGconn* conn) {
    if (!conn) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_shutdown) {
        --m_inUse;
        PQfinish(conn);
        m_cv.notify_one();
        return;
    }
    if (!isConnectionHealthy(conn)) {
        PQfinish(conn);
        --m_total;
        --m_inUse;
        m_cv.notify_one();
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
        PQfinish(c);

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
}
