#ifndef CMYSQL_H
#define CMYSQL_H

#include <libpq-fe.h>
#include <list>
#include <memory>
#include <string>
#include <vector>

class MySqlConnGuard;

class CMySql {
public:
    struct Param {
        enum class Type {
            Null,
            SignedInteger,
            UnsignedInteger,
            FloatingPoint,
            String
        };

        Param() = default;
        Param(const char* value);
        Param(const std::string& value);
        Param(std::string&& value);
        Param(int value);
        Param(long long value);
        Param(unsigned int value);
        Param(unsigned long long value);
        Param(float value);
        Param(double value);

        static Param null();

        Type type = Type::Null;
        std::string stringValue;
        long long signedValue = 0;
        unsigned long long unsignedValue = 0;
        double floatingValue = 0.0;
    };

    CMySql();
    ~CMySql();

    CMySql(const CMySql&) = delete;
    CMySql& operator=(const CMySql&) = delete;

    bool SelectMySql(const char* sql,
                     int nColumn,
                     std::list<std::string>& out);
    bool SelectPrepared(const char* sql,
                        const std::vector<Param>& params,
                        int nColumn,
                        std::list<std::string>& out);

    bool UpdateMySql(const char* sql);
    bool UpdatePrepared(const char* sql, const std::vector<Param>& params);
    bool BeginTransaction();
    bool CommitTransaction();
    bool RollbackTransaction();
    unsigned long long LastInsertId() const;

private:
    bool ensureConnection();
    void releaseConnectionIfIdle();

private:
    std::unique_ptr<MySqlConnGuard> m_guard;
    PGconn* m_conn = nullptr;
    bool m_inTransaction = false;
    unsigned long long m_lastInsertId = 0;
};

#endif
