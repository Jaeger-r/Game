#include "CMySql.h"

#include "mysqlconnpool.h"

#include <QDebug>
#include <QString>

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
struct PgResultDeleter {
    void operator()(PGresult* result) const
    {
        if (result) {
            PQclear(result);
        }
    }
};

using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

std::string buildParameterizedQuery(const char* sql)
{
    if (!sql) {
        return {};
    }

    std::string query(sql);
    std::string converted;
    converted.reserve(query.size() + 16);

    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    int paramIndex = 0;

    for (size_t index = 0; index < query.size(); ++index) {
        const char ch = query[index];

        if (ch == '\'' && !inDoubleQuote) {
            converted.push_back(ch);
            if (inSingleQuote && index + 1 < query.size() && query[index + 1] == '\'') {
                converted.push_back(query[index + 1]);
                ++index;
                continue;
            }
            inSingleQuote = !inSingleQuote;
            continue;
        }

        if (ch == '"' && !inSingleQuote) {
            converted.push_back(ch);
            if (inDoubleQuote && index + 1 < query.size() && query[index + 1] == '"') {
                converted.push_back(query[index + 1]);
                ++index;
                continue;
            }
            inDoubleQuote = !inDoubleQuote;
            continue;
        }

        if (ch == '?' && !inSingleQuote && !inDoubleQuote) {
            ++paramIndex;
            converted.push_back('$');
            converted += std::to_string(paramIndex);
            continue;
        }

        converted.push_back(ch);
    }

    return converted;
}

std::string paramToText(const CMySql::Param& param)
{
    switch (param.type) {
    case CMySql::Param::Type::Null:
        return {};
    case CMySql::Param::Type::SignedInteger:
        return std::to_string(param.signedValue);
    case CMySql::Param::Type::UnsignedInteger:
        return std::to_string(param.unsignedValue);
    case CMySql::Param::Type::FloatingPoint:
        return std::to_string(param.floatingValue);
    case CMySql::Param::Type::String:
        return param.stringValue;
    }

    return {};
}

bool isCommandResultOk(ExecStatusType status)
{
    return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;
}

void updateLastInsertIdFromResult(PGresult* result, unsigned long long* lastInsertId)
{
    if (!lastInsertId) {
        return;
    }

    *lastInsertId = 0;
    if (!result || PQresultStatus(result) != PGRES_TUPLES_OK
        || PQntuples(result) < 1
        || PQnfields(result) < 1)
    {
        return;
    }

    bool ok = false;
    const QString value = QString::fromUtf8(PQgetvalue(result, 0, 0)).trimmed();
    const qulonglong parsed = value.toULongLong(&ok);
    if (ok) {
        *lastInsertId = parsed;
    }
}
} // namespace

CMySql::Param::Param(const char* value)
    : type(Type::String)
    , stringValue(value ? value : "")
{
}

CMySql::Param::Param(const std::string& value)
    : type(Type::String)
    , stringValue(value)
{
}

CMySql::Param::Param(std::string&& value)
    : type(Type::String)
    , stringValue(std::move(value))
{
}

CMySql::Param::Param(int value)
    : type(Type::SignedInteger)
    , signedValue(value)
{
}

CMySql::Param::Param(long long value)
    : type(Type::SignedInteger)
    , signedValue(value)
{
}

CMySql::Param::Param(unsigned int value)
    : type(Type::UnsignedInteger)
    , unsignedValue(value)
{
}

CMySql::Param::Param(unsigned long long value)
    : type(Type::UnsignedInteger)
    , unsignedValue(value)
{
}

CMySql::Param::Param(float value)
    : type(Type::FloatingPoint)
    , floatingValue(static_cast<double>(value))
{
}

CMySql::Param::Param(double value)
    : type(Type::FloatingPoint)
    , floatingValue(value)
{
}

CMySql::Param CMySql::Param::null()
{
    return Param();
}

CMySql::CMySql() = default;

CMySql::~CMySql()
{
    if (m_inTransaction) {
        RollbackTransaction();
    }
    m_guard.reset();
    m_conn = nullptr;
}

bool CMySql::ensureConnection()
{
    if (m_conn) {
        return true;
    }

    m_guard = std::make_unique<MySqlConnGuard>();
    m_conn = m_guard ? m_guard->get() : nullptr;
    return m_conn != nullptr;
}

void CMySql::releaseConnectionIfIdle()
{
    if (m_inTransaction) {
        return;
    }
    m_conn = nullptr;
    m_guard.reset();
}

bool CMySql::SelectMySql(const char* sql,
                         int nColumn,
                         std::list<std::string>& out)
{
    if (!sql) {
        return false;
    }

    if (!ensureConnection()) {
        return false;
    }

    PgResultPtr result(PQexec(m_conn, sql));
    if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
        qWarning() << "PostgreSQL query failed:" << QString::fromLocal8Bit(PQerrorMessage(m_conn)).trimmed();
        releaseConnectionIfIdle();
        return false;
    }

    if (PQnfields(result.get()) < nColumn) {
        qWarning() << "PostgreSQL query returned fewer columns than expected."
                   << PQnfields(result.get())
                   << "<"
                   << nColumn;
        releaseConnectionIfIdle();
        return false;
    }

    const int rows = PQntuples(result.get());
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < nColumn; ++column) {
            if (PQgetisnull(result.get(), row, column)) {
                out.emplace_back();
            } else {
                out.emplace_back(PQgetvalue(result.get(), row, column));
            }
        }
    }

    releaseConnectionIfIdle();
    return true;
}

bool CMySql::SelectPrepared(const char* sql,
                            const std::vector<Param>& params,
                            int nColumn,
                            std::list<std::string>& out)
{
    if (!sql) {
        return false;
    }

    if (!ensureConnection()) {
        return false;
    }

    const std::string convertedQuery = buildParameterizedQuery(sql);
    std::vector<std::string> paramStorage;
    std::vector<const char*> paramValues;
    paramStorage.reserve(params.size());
    paramValues.reserve(params.size());

    for (const Param& param : params) {
        if (param.type == Param::Type::Null) {
            paramStorage.emplace_back();
            paramValues.push_back(nullptr);
            continue;
        }

        paramStorage.push_back(paramToText(param));
        paramValues.push_back(paramStorage.back().c_str());
    }

    PgResultPtr result(PQexecParams(m_conn,
                                    convertedQuery.c_str(),
                                    static_cast<int>(params.size()),
                                    nullptr,
                                    paramValues.empty() ? nullptr : paramValues.data(),
                                    nullptr,
                                    nullptr,
                                    0));
    if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
        qWarning() << "PostgreSQL prepared query failed:"
                   << QString::fromLocal8Bit(PQerrorMessage(m_conn)).trimmed();
        releaseConnectionIfIdle();
        return false;
    }

    if (PQnfields(result.get()) != nColumn) {
        qWarning() << "Prepared select column count mismatch. Expected"
                   << nColumn
                   << "but got"
                   << PQnfields(result.get());
        releaseConnectionIfIdle();
        return false;
    }

    const int rows = PQntuples(result.get());
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < nColumn; ++column) {
            if (PQgetisnull(result.get(), row, column)) {
                out.emplace_back();
            } else {
                out.emplace_back(PQgetvalue(result.get(), row, column));
            }
        }
    }

    releaseConnectionIfIdle();
    return true;
}

bool CMySql::UpdateMySql(const char* sql)
{
    if (!sql) {
        return false;
    }

    if (!ensureConnection()) {
        return false;
    }
    m_lastInsertId = 0;

    PgResultPtr result(PQexec(m_conn, sql));
    if (!result || !isCommandResultOk(PQresultStatus(result.get()))) {
        qWarning() << "PostgreSQL update failed:" << QString::fromLocal8Bit(PQerrorMessage(m_conn)).trimmed();
        releaseConnectionIfIdle();
        return false;
    }

    updateLastInsertIdFromResult(result.get(), &m_lastInsertId);
    releaseConnectionIfIdle();
    return true;
}

bool CMySql::UpdatePrepared(const char* sql, const std::vector<Param>& params)
{
    if (!sql) {
        return false;
    }

    if (!ensureConnection()) {
        return false;
    }
    m_lastInsertId = 0;

    const std::string convertedQuery = buildParameterizedQuery(sql);
    std::vector<std::string> paramStorage;
    std::vector<const char*> paramValues;
    paramStorage.reserve(params.size());
    paramValues.reserve(params.size());

    for (const Param& param : params) {
        if (param.type == Param::Type::Null) {
            paramStorage.emplace_back();
            paramValues.push_back(nullptr);
            continue;
        }

        paramStorage.push_back(paramToText(param));
        paramValues.push_back(paramStorage.back().c_str());
    }

    PgResultPtr result(PQexecParams(m_conn,
                                    convertedQuery.c_str(),
                                    static_cast<int>(params.size()),
                                    nullptr,
                                    paramValues.empty() ? nullptr : paramValues.data(),
                                    nullptr,
                                    nullptr,
                                    0));
    if (!result || !isCommandResultOk(PQresultStatus(result.get()))) {
        qWarning() << "PostgreSQL prepared update failed:"
                   << QString::fromLocal8Bit(PQerrorMessage(m_conn)).trimmed();
        releaseConnectionIfIdle();
        return false;
    }

    updateLastInsertIdFromResult(result.get(), &m_lastInsertId);
    releaseConnectionIfIdle();
    return true;
}

bool CMySql::BeginTransaction()
{
    if (m_inTransaction) {
        return true;
    }
    if (!ensureConnection()) {
        return false;
    }

    PgResultPtr result(PQexec(m_conn, "BEGIN;"));
    if (!result || PQresultStatus(result.get()) != PGRES_COMMAND_OK) {
        qWarning() << "PostgreSQL begin transaction failed:"
                   << QString::fromLocal8Bit(PQerrorMessage(m_conn)).trimmed();
        releaseConnectionIfIdle();
        return false;
    }

    m_inTransaction = true;
    return true;
}

bool CMySql::CommitTransaction()
{
    if (!m_inTransaction || !m_conn) {
        return true;
    }

    PgResultPtr result(PQexec(m_conn, "COMMIT;"));
    const bool ok = result && PQresultStatus(result.get()) == PGRES_COMMAND_OK;
    if (!ok) {
        qWarning() << "PostgreSQL commit failed:"
                   << QString::fromLocal8Bit(PQerrorMessage(m_conn)).trimmed();
    }

    m_inTransaction = false;
    releaseConnectionIfIdle();
    return ok;
}

bool CMySql::RollbackTransaction()
{
    if (!m_inTransaction || !m_conn) {
        return true;
    }

    PgResultPtr result(PQexec(m_conn, "ROLLBACK;"));
    const bool ok = result && PQresultStatus(result.get()) == PGRES_COMMAND_OK;
    if (!ok) {
        qWarning() << "PostgreSQL rollback failed:"
                   << QString::fromLocal8Bit(PQerrorMessage(m_conn)).trimmed();
    }

    m_inTransaction = false;
    releaseConnectionIfIdle();
    return ok;
}

unsigned long long CMySql::LastInsertId() const
{
    return m_lastInsertId;
}
