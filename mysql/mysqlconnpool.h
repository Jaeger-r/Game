#ifndef MYSQLCONNPOOL_H
#define MYSQLCONNPOOL_H
#pragma once

#include "/usr/local/mysql-8.0.35-macos13-arm64/include/mysql.h"
#include <vector>
#include <mutex>
#include <condition_variable>
#include <string>

/*
 * 线程安全 MySQL 连接池
 */
class MySqlConnPool {
public:
    static MySqlConnPool& Instance();

    void init(const std::string& host,
              const std::string& user,
              const std::string& pass,
              const std::string& db,
              int poolSize);

    MYSQL* acquire();
    void release(MYSQL* conn);

    ~MySqlConnPool();

private:
    MySqlConnPool() = default;
    MySqlConnPool(const MySqlConnPool&) = delete;
    MySqlConnPool& operator=(const MySqlConnPool&) = delete;

    MYSQL* createConnection();
private:
    std::vector<MYSQL*> m_idle;   // 空闲连接
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
};

/*
 * RAII 连接守卫
 */
class MySqlConnGuard {
public:
    MySqlConnGuard();
    ~MySqlConnGuard();

    MYSQL* get() { return m_conn; }

private:
    MYSQL* m_conn = nullptr;
};

#endif
