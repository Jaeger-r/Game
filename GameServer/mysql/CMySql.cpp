#include "CMySql.h"
#include "MySqlConnPool.h"
#include <QDebug>

CMySql::CMySql() = default;
CMySql::~CMySql() = default;

/* ================= 查询 ================= */

bool CMySql::SelectMySql(const char* sql,
                         int nColumn,
                         std::list<std::string>& out)
{
    if (!sql) return false;

    MySqlConnGuard guard;
    MYSQL* conn = guard.get();
    if (!conn) return false;

    if (mysql_query(conn, sql)) {
        qDebug() << "MySQL error:" << mysql_error(conn);
        return false;
    }

    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) {
        qDebug() << "MySQL store result failed:" << mysql_error(conn);
        return false;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        for (int i = 0; i < nColumn; ++i) {
            out.push_back(row[i] ? row[i] : "");
        }
    }

    mysql_free_result(res);
    return true;
}

/* ================= 更新 ================= */

bool CMySql::UpdateMySql(const char* sql)
{
    if (!sql) return false;

    MySqlConnGuard guard;
    MYSQL* conn = guard.get();
    if (!conn) return false;

    if (mysql_query(conn, sql)) {
        qDebug() << "MySQL error:" << mysql_error(conn);
        return false;
    }

    return true;
}
