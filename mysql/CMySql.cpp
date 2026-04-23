#include "CMySql.h"
#include "mysqlconnpool.h"
#include <QDebug>
#include <algorithm>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using MysqlBindBool =
    std::remove_pointer_t<decltype(std::declval<MYSQL_BIND&>().is_null)>;
using MysqlErrorBool =
    std::remove_pointer_t<decltype(std::declval<MYSQL_BIND&>().error)>;

struct BoundParamStorage {
    MYSQL_BIND bind{};
    MysqlBindBool isNull = 0;
    std::string stringValue;
    unsigned long stringLength = 0;
    long long signedValue = 0;
    unsigned long long unsignedValue = 0;
    double floatingValue = 0.0;
};

struct ResultColumnStorage {
    MYSQL_BIND bind{};
    MysqlBindBool isNull = 0;
    MysqlErrorBool error = 0;
    std::vector<char> buffer;
    unsigned long length = 0;
};

bool bindPreparedParams(MYSQL_STMT* stmt,
                        const std::vector<CMySql::Param>& params,
                        std::vector<BoundParamStorage>* boundParams)
{
    if (!stmt || !boundParams) {
        return false;
    }

    const unsigned long expectedCount = mysql_stmt_param_count(stmt);
    if (expectedCount != static_cast<unsigned long>(params.size())) {
        qWarning() << "Prepared statement parameter count mismatch. Expected"
                   << expectedCount
                   << "but got"
                   << params.size();
        return false;
    }

    boundParams->assign(params.size(), BoundParamStorage{});
    std::vector<MYSQL_BIND> binds(params.size());

    for (size_t index = 0; index < params.size(); ++index) {
        const CMySql::Param& param = params[index];
        BoundParamStorage& slot = (*boundParams)[index];

        switch (param.type) {
        case CMySql::Param::Type::Null:
            slot.isNull = 1;
            slot.bind.buffer_type = MYSQL_TYPE_NULL;
            slot.bind.is_null = &slot.isNull;
            break;
        case CMySql::Param::Type::SignedInteger:
            slot.signedValue = param.signedValue;
            slot.bind.buffer_type = MYSQL_TYPE_LONGLONG;
            slot.bind.buffer = &slot.signedValue;
            slot.bind.is_unsigned = 0;
            slot.bind.is_null = &slot.isNull;
            break;
        case CMySql::Param::Type::UnsignedInteger:
            slot.unsignedValue = param.unsignedValue;
            slot.bind.buffer_type = MYSQL_TYPE_LONGLONG;
            slot.bind.buffer = &slot.unsignedValue;
            slot.bind.is_unsigned = 1;
            slot.bind.is_null = &slot.isNull;
            break;
        case CMySql::Param::Type::FloatingPoint:
            slot.floatingValue = param.floatingValue;
            slot.bind.buffer_type = MYSQL_TYPE_DOUBLE;
            slot.bind.buffer = &slot.floatingValue;
            slot.bind.is_null = &slot.isNull;
            break;
        case CMySql::Param::Type::String:
            slot.stringValue = param.stringValue;
            slot.stringLength =
                static_cast<unsigned long>(slot.stringValue.size());
            slot.bind.buffer_type = MYSQL_TYPE_STRING;
            slot.bind.buffer = const_cast<char*>(slot.stringValue.data());
            slot.bind.buffer_length = slot.stringLength;
            slot.bind.length = &slot.stringLength;
            slot.bind.is_null = &slot.isNull;
            break;
        }

        binds[index] = slot.bind;
    }

    if (binds.empty()) {
        return true;
    }

    if (mysql_stmt_bind_param(stmt, binds.data()) != 0) {
        qWarning() << "mysql_stmt_bind_param failed:" << mysql_stmt_error(stmt);
        return false;
    }

    return true;
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

/* ================= 查询 ================= */

/**
 * @brief 处理SelectMySql相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool CMySql::SelectMySql(const char* sql,
                         int nColumn,
                         std::list<std::string>& out)
{
    if (!sql) return false;

    if (!ensureConnection()) return false;

    if (mysql_query(m_conn, sql)) {
        qDebug() << "MySQL error:" << mysql_error(m_conn);
        releaseConnectionIfIdle();
        return false;
    }

    MYSQL_RES* res = mysql_store_result(m_conn);
    if (!res) {
        qDebug() << "MySQL store result failed:" << mysql_error(m_conn);
        releaseConnectionIfIdle();
        return false;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        for (int i = 0; i < nColumn; ++i) {
            out.push_back(row[i] ? row[i] : "");
        }
    }

    mysql_free_result(res);
    releaseConnectionIfIdle();
    return true;
}

bool CMySql::SelectPrepared(const char* sql,
                            const std::vector<Param>& params,
                            int nColumn,
                            std::list<std::string>& out)
{
    if (!sql) return false;

    if (!ensureConnection()) return false;

    auto stmtCloser = [](MYSQL_STMT* stmt) {
        if (stmt) {
            mysql_stmt_close(stmt);
        }
    };
    auto metadataCloser = [](MYSQL_RES* metadata) {
        if (metadata) {
            mysql_free_result(metadata);
        }
    };

    std::unique_ptr<MYSQL_STMT, decltype(stmtCloser)> stmt(mysql_stmt_init(m_conn),
                                                           stmtCloser);
    if (!stmt) {
        qWarning() << "mysql_stmt_init failed for prepared select.";
        releaseConnectionIfIdle();
        return false;
    }

    if (mysql_stmt_prepare(stmt.get(), sql, static_cast<unsigned long>(std::strlen(sql))) != 0) {
        qWarning() << "mysql_stmt_prepare failed:" << mysql_stmt_error(stmt.get());
        releaseConnectionIfIdle();
        return false;
    }

    std::vector<BoundParamStorage> boundParams;
    if (!bindPreparedParams(stmt.get(), params, &boundParams)) {
        releaseConnectionIfIdle();
        return false;
    }

    MysqlBindBool updateMaxLength = 1;
    if (mysql_stmt_attr_set(stmt.get(),
                            STMT_ATTR_UPDATE_MAX_LENGTH,
                            &updateMaxLength) != 0) {
        qWarning() << "mysql_stmt_attr_set(STMT_ATTR_UPDATE_MAX_LENGTH) failed:"
                   << mysql_stmt_error(stmt.get());
        releaseConnectionIfIdle();
        return false;
    }

    if (mysql_stmt_execute(stmt.get()) != 0) {
        qWarning() << "mysql_stmt_execute failed:" << mysql_stmt_error(stmt.get());
        releaseConnectionIfIdle();
        return false;
    }

    if (mysql_stmt_store_result(stmt.get()) != 0) {
        qWarning() << "mysql_stmt_store_result failed:" << mysql_stmt_error(stmt.get());
        releaseConnectionIfIdle();
        return false;
    }

    std::unique_ptr<MYSQL_RES, decltype(metadataCloser)> metadata(
        mysql_stmt_result_metadata(stmt.get()),
        metadataCloser);
    if (!metadata) {
        if (mysql_stmt_field_count(stmt.get()) == 0) {
            releaseConnectionIfIdle();
            return true;
        }
        qWarning() << "mysql_stmt_result_metadata failed:" << mysql_stmt_error(stmt.get());
        releaseConnectionIfIdle();
        return false;
    }

    const unsigned int fieldCount = mysql_num_fields(metadata.get());
    if (fieldCount != static_cast<unsigned int>(nColumn)) {
        qWarning() << "Prepared select column count mismatch. Expected"
                   << nColumn
                   << "but got"
                   << fieldCount;
        releaseConnectionIfIdle();
        return false;
    }

    MYSQL_FIELD* fields = mysql_fetch_fields(metadata.get());
    std::vector<ResultColumnStorage> columns(fieldCount);
    std::vector<MYSQL_BIND> resultBinds(fieldCount);

    for (unsigned int index = 0; index < fieldCount; ++index) {
        ResultColumnStorage& column = columns[index];
        const unsigned long bufferSize =
            std::max<unsigned long>(fields[index].max_length + 1, 1UL);
        column.buffer.resize(static_cast<size_t>(bufferSize));
        column.bind.buffer_type = MYSQL_TYPE_STRING;
        column.bind.buffer = column.buffer.data();
        column.bind.buffer_length =
            static_cast<unsigned long>(column.buffer.size());
        column.bind.length = &column.length;
        column.bind.is_null = &column.isNull;
        column.bind.error = &column.error;
        resultBinds[index] = column.bind;
    }

    if (!resultBinds.empty() && mysql_stmt_bind_result(stmt.get(), resultBinds.data()) != 0) {
        qWarning() << "mysql_stmt_bind_result failed:" << mysql_stmt_error(stmt.get());
        releaseConnectionIfIdle();
        return false;
    }

    while (true) {
        const int fetchStatus = mysql_stmt_fetch(stmt.get());
        if (fetchStatus == MYSQL_NO_DATA) {
            break;
        }
        if (fetchStatus != 0 && fetchStatus != MYSQL_DATA_TRUNCATED) {
            qWarning() << "mysql_stmt_fetch failed:" << mysql_stmt_error(stmt.get());
            releaseConnectionIfIdle();
            return false;
        }

        if (fetchStatus == MYSQL_DATA_TRUNCATED) {
            for (unsigned int index = 0; index < fieldCount; ++index) {
                ResultColumnStorage& column = columns[index];
                if (column.isNull || !column.error) {
                    continue;
                }
                const unsigned long requiredSize =
                    std::max<unsigned long>(column.length + 1, 1UL);
                if (requiredSize <= column.buffer.size()) {
                    continue;
                }

                column.buffer.resize(static_cast<size_t>(requiredSize));
                column.bind.buffer = column.buffer.data();
                column.bind.buffer_length =
                    static_cast<unsigned long>(column.buffer.size());
                resultBinds[index] = column.bind;

                if (mysql_stmt_fetch_column(stmt.get(), &resultBinds[index], index, 0) != 0) {
                    qWarning() << "mysql_stmt_fetch_column failed:"
                               << mysql_stmt_error(stmt.get());
                    releaseConnectionIfIdle();
                    return false;
                }
            }
        }

        for (const ResultColumnStorage& column : columns) {
            if (column.isNull) {
                out.emplace_back();
            } else {
                out.emplace_back(column.buffer.data(),
                                 static_cast<size_t>(column.length));
            }
        }
    }

    releaseConnectionIfIdle();
    return true;
}

/* ================= 更新 ================= */

/**
 * @brief 更新UpdateMySql相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool CMySql::UpdateMySql(const char* sql)
{
    if (!sql) return false;

    if (!ensureConnection()) return false;
    m_lastInsertId = 0;

    if (mysql_query(m_conn, sql)) {
        qDebug() << "MySQL error:" << mysql_error(m_conn);
        releaseConnectionIfIdle();
        return false;
    }

    m_lastInsertId = mysql_insert_id(m_conn);
    releaseConnectionIfIdle();
    return true;
}

bool CMySql::UpdatePrepared(const char* sql, const std::vector<Param>& params)
{
    if (!sql) return false;

    if (!ensureConnection()) return false;
    m_lastInsertId = 0;

    auto stmtCloser = [](MYSQL_STMT* stmt) {
        if (stmt) {
            mysql_stmt_close(stmt);
        }
    };

    std::unique_ptr<MYSQL_STMT, decltype(stmtCloser)> stmt(mysql_stmt_init(m_conn),
                                                           stmtCloser);
    if (!stmt) {
        qWarning() << "mysql_stmt_init failed for prepared update.";
        releaseConnectionIfIdle();
        return false;
    }

    if (mysql_stmt_prepare(stmt.get(), sql, static_cast<unsigned long>(std::strlen(sql))) != 0) {
        qWarning() << "mysql_stmt_prepare failed:" << mysql_stmt_error(stmt.get());
        releaseConnectionIfIdle();
        return false;
    }

    std::vector<BoundParamStorage> boundParams;
    if (!bindPreparedParams(stmt.get(), params, &boundParams)) {
        releaseConnectionIfIdle();
        return false;
    }

    if (mysql_stmt_execute(stmt.get()) != 0) {
        qWarning() << "mysql_stmt_execute failed:" << mysql_stmt_error(stmt.get());
        releaseConnectionIfIdle();
        return false;
    }

    m_lastInsertId = mysql_stmt_insert_id(stmt.get());
    releaseConnectionIfIdle();
    return true;
}

/**
 * @brief 开启BeginTransaction相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool CMySql::BeginTransaction()
{
    if (m_inTransaction) {
        return true;
    }
    if (!ensureConnection()) {
        return false;
    }
    if (mysql_autocommit(m_conn, 0) != 0) {
        qDebug() << "MySQL begin transaction failed:" << mysql_error(m_conn);
        releaseConnectionIfIdle();
        return false;
    }
    m_inTransaction = true;
    return true;
}

/**
 * @brief 提交CommitTransaction相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool CMySql::CommitTransaction()
{
    if (!m_inTransaction || !m_conn) {
        return true;
    }
    const bool ok = mysql_commit(m_conn) == 0;
    if (!ok) {
        qDebug() << "MySQL commit failed:" << mysql_error(m_conn);
    }
    mysql_autocommit(m_conn, 1);
    m_inTransaction = false;
    releaseConnectionIfIdle();
    return ok;
}

/**
 * @brief 回滚RollbackTransaction相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool CMySql::RollbackTransaction()
{
    if (!m_inTransaction || !m_conn) {
        return true;
    }
    const bool ok = mysql_rollback(m_conn) == 0;
    if (!ok) {
        qDebug() << "MySQL rollback failed:" << mysql_error(m_conn);
    }
    mysql_autocommit(m_conn, 1);
    m_inTransaction = false;
    releaseConnectionIfIdle();
    return ok;
}

unsigned long long CMySql::LastInsertId() const
{
    return m_lastInsertId;
}
