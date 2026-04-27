#ifndef MYSQLCONNPOOL_H
#define MYSQLCONNPOOL_H
#pragma once

#include <libpq-fe.h>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

/*
 * 线程安全 PostgreSQL 连接池。
 * 为了减少业务层改动，暂时保留了原有类型名。
 */
class MySqlConnPool {
public:
    static MySqlConnPool& Instance();

    void init(const std::string& host,
              const std::string& user,
              const std::string& pass,
              const std::string& db,
              unsigned int port,
              int poolSize);

    PGconn* acquire();
    void release(PGconn* conn);

    ~MySqlConnPool();

private:
    MySqlConnPool() = default;
    MySqlConnPool(const MySqlConnPool&) = delete;
    MySqlConnPool& operator=(const MySqlConnPool&) = delete;

    PGconn* createConnection();
    bool isConnectionHealthy(PGconn* conn) const;
private:
    std::vector<PGconn*> m_idle;
    int m_total = 0;
    int m_inUse = 0;

    bool m_shutdown = false;
    bool m_inited = false;

    std::mutex m_mutex;
    std::condition_variable m_cv;

    std::string m_host;
    std::string m_user;
    std::string m_pass;
    std::string m_db;
    unsigned int m_port = 5432;
    int m_targetPoolSize = 0;
};

/*
 * RAII 连接守卫
 */
class MySqlConnGuard {
public:
    MySqlConnGuard();
    ~MySqlConnGuard();

    PGconn* get() { return m_conn; }

private:
    PGconn* m_conn = nullptr;
};

#endif
