#ifndef CMYSQL_H
#define CMYSQL_H

#include "/usr/local/mysql-8.0.35-macos13-arm64/include/mysql.h"
#include <list>
#include <string>

class CMySql {
public:
    CMySql();
    ~CMySql();

    CMySql(const CMySql&) = delete;
    CMySql& operator=(const CMySql&) = delete;

    bool SelectMySql(const char* sql,
                     int nColumn,
                     std::list<std::string>& out);

    bool UpdateMySql(const char* sql);
};

#endif
