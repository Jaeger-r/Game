#include "core.h"
#include <QDateTime>
#include <QCryptographicHash>
#include <QTextStream>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QRandomGenerator>
#include <QStringList>
#include <cstring>
#include <limits>
#include <utility>
#include <tinyxml2.h>
#include "qkcpsocket.h"
#include "sharedgamecontent.h"
#include "sharedpaths.h"

using namespace tinyxml2;

#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define RESET   "\033[0m"

namespace {
QString g_configuredSharedDataDirectory;

constexpr int kServerPlayerSize = 30;
constexpr int kServerNpcSize = 30;
constexpr int kEquipmentWeaponSlotIndex = 0;
constexpr int kEnchantKindNone = 0;
constexpr int kEnchantKindAttack = 1;
constexpr int kEnchantKindDefence = 2;
constexpr int kEnchantKindVitality = 3;
constexpr int kEnchantKindCritical = 4;

using ServerItemKind = JaegerShared::ContentItemKind;
using ServerItemMetadata = JaegerShared::ContentItemMetadata;
using InventoryEnchantValues = JaegerShared::InventoryEnchantValues;
using InventoryRulesConfig = JaegerShared::InventoryRulesConfig;
using RewardParticipationRule = JaegerShared::RewardParticipationRule;
using RewardRulesConfig = JaegerShared::RewardRulesConfig;
using SqlParam = CMySql::Param;

using ItemCountMap = QMap<int, int>;

bool selectQuery(CMySql& sql, const QString& query, int nColumn, std::list<std::string>& out)
{
    const QByteArray utf8 = query.toUtf8();
    return sql.SelectMySql(utf8.constData(), nColumn, out);
}

bool updateQuery(CMySql& sql, const QString& query)
{
    const QByteArray utf8 = query.toUtf8();
    return sql.UpdateMySql(utf8.constData());
}

bool selectPreparedQuery(CMySql& sql,
                         const char* query,
                         const std::vector<SqlParam>& params,
                         int nColumn,
                         std::list<std::string>& out)
{
    return sql.SelectPrepared(query, params, nColumn, out);
}

bool selectPreparedQuery(CMySql& sql,
                         const QString& query,
                         const std::vector<SqlParam>& params,
                         int nColumn,
                         std::list<std::string>& out)
{
    const QByteArray utf8 = query.toUtf8();
    return sql.SelectPrepared(utf8.constData(), params, nColumn, out);
}

bool updatePreparedQuery(CMySql& sql,
                         const char* query,
                         const std::vector<SqlParam>& params)
{
    return sql.UpdatePrepared(query, params);
}

bool updatePreparedQuery(CMySql& sql,
                         const QString& query,
                         const std::vector<SqlParam>& params)
{
    const QByteArray utf8 = query.toUtf8();
    return sql.UpdatePrepared(utf8.constData(), params);
}

std::string utf8StdString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return std::string(utf8.constData(), static_cast<size_t>(utf8.size()));
}

template <typename... Args>
std::vector<SqlParam> sqlParams(Args&&... args)
{
    std::vector<SqlParam> params;
    params.reserve(sizeof...(Args));
    (params.emplace_back(std::forward<Args>(args)), ...);
    return params;
}

QString escapedSqlString(QString value)
{
    value.replace('\'', QStringLiteral("''"));
    return value;
}

QString quotedIdentifier(QString value)
{
    value.replace('"', QStringLiteral("\"\""));
    return QStringLiteral("\"%1\"").arg(value);
}

bool querySchemaColumnExists(CMySql& sql,
                             const QString& tableName,
                             const QString& columnName,
                             bool* exists)
{
    std::list<std::string> result;
    const QString query =
        QStringLiteral("SELECT COUNT(*) FROM information_schema.columns "
                       "WHERE table_schema = current_schema() "
                       "AND TABLE_NAME = '%1' "
                       "AND COLUMN_NAME = '%2';")
            .arg(escapedSqlString(tableName), escapedSqlString(columnName));
    if (!selectQuery(sql, query, 1, result)) {
        return false;
    }

    if (exists) {
        *exists = !result.empty() && result.front() != "0";
    }
    return true;
}

bool ensureSchemaColumnExists(CMySql& sql,
                              const QString& tableName,
                              const QString& columnName,
                              const QString& columnDefinition)
{
    bool exists = false;
    if (!querySchemaColumnExists(sql, tableName, columnName, &exists)) {
        return false;
    }
    if (exists) {
        return true;
    }

    const QString alterSql =
        QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3;")
            .arg(quotedIdentifier(tableName), quotedIdentifier(columnName), columnDefinition);
    return updateQuery(sql, alterSql);
}

bool querySchemaIndexExists(CMySql& sql,
                            const QString& tableName,
                            const QString& indexName,
                            bool* exists)
{
    std::list<std::string> result;
    const QString query =
        QStringLiteral("SELECT COUNT(*) FROM pg_indexes "
                       "WHERE schemaname = current_schema() "
                       "AND tablename = '%1' "
                       "AND indexname = '%2';")
            .arg(escapedSqlString(tableName), escapedSqlString(indexName));
    if (!selectQuery(sql, query, 1, result)) {
        return false;
    }

    if (exists) {
        *exists = !result.empty() && result.front() != "0";
    }
    return true;
}

bool ensureSchemaIndexExists(CMySql& sql,
                             const QString& tableName,
                             const QString& indexName,
                             const QString& indexDefinition)
{
    bool exists = false;
    if (!querySchemaIndexExists(sql, tableName, indexName, &exists)) {
        return false;
    }
    if (exists) {
        return true;
    }

    const QString alterSql =
        QStringLiteral("CREATE INDEX %1 ON %2 %3;")
            .arg(quotedIdentifier(indexName), quotedIdentifier(tableName), indexDefinition);
    return updateQuery(sql, alterSql);
}

constexpr const char* kInventoryJournalStatusPending = "pending";
constexpr const char* kInventoryJournalStatusApplied = "applied";
constexpr const char* kInventoryJournalStatusSuperseded = "superseded";
constexpr const char* kInventoryJournalStatusFailed = "failed";

QVector<QPair<int, int>> normalizedPersistEntries(const QVector<QPair<int, int>>& rawEntries);

qint64 jsonValueToLongLong(const QJsonValue& value, qint64 fallback = 0)
{
    if (value.isString()) {
        bool ok = false;
        const qint64 parsed = value.toString().toLongLong(&ok);
        return ok ? parsed : fallback;
    }
    if (value.isDouble()) {
        return static_cast<qint64>(value.toDouble(static_cast<double>(fallback)));
    }
    return fallback;
}

int jsonValueToInt(const QJsonValue& value, int fallback = 0)
{
    if (value.isDouble()) {
        return value.toInt(fallback);
    }
    if (value.isString()) {
        bool ok = false;
        const int parsed = value.toString().toInt(&ok);
        return ok ? parsed : fallback;
    }
    return fallback;
}

float jsonValueToFloat(const QJsonValue& value, float fallback = 0.0f)
{
    if (value.isDouble()) {
        return static_cast<float>(value.toDouble(static_cast<double>(fallback)));
    }
    if (value.isString()) {
        bool ok = false;
        const float parsed = value.toString().toFloat(&ok);
        return ok ? parsed : fallback;
    }
    return fallback;
}

QJsonArray inventoryJournalItemEntriesToJson(const QVector<QPair<int, int>>& entries)
{
    QJsonArray array;
    for (const auto& entry : entries) {
        if (entry.first <= 0 || entry.second <= 0) {
            continue;
        }

        QJsonObject itemObject;
        itemObject.insert(QStringLiteral("itemId"), entry.first);
        itemObject.insert(QStringLiteral("count"), entry.second);
        array.append(itemObject);
    }
    return array;
}

QVector<QPair<int, int>> inventoryJournalItemEntriesFromJson(const QJsonArray& array)
{
    QVector<QPair<int, int>> entries;
    entries.reserve(array.size());
    for (const QJsonValue& value : array) {
        const QJsonObject itemObject = value.toObject();
        const int itemId = jsonValueToInt(itemObject.value(QStringLiteral("itemId")));
        const int count = jsonValueToInt(itemObject.value(QStringLiteral("count")));
        if (itemId <= 0 || count <= 0) {
            continue;
        }
        entries.push_back(qMakePair(itemId, count));
    }
    return entries;
}

QJsonArray inventoryJournalEquipmentStateToJson(const Player_Information& playerInfo)
{
    QJsonArray array;
    for (int slotIndex = 0; slotIndex < MAX_EQUIPMENT_SLOT_NUM; ++slotIndex) {
        QJsonObject slotObject;
        slotObject.insert(QStringLiteral("slotIndex"), slotIndex);
        slotObject.insert(QStringLiteral("itemId"), playerInfo.equippedItemIds[slotIndex]);
        slotObject.insert(QStringLiteral("enhanceLevel"), playerInfo.equippedEnhanceLevels[slotIndex]);
        slotObject.insert(QStringLiteral("forgeLevel"), playerInfo.equippedForgeLevels[slotIndex]);
        slotObject.insert(QStringLiteral("enchantKind"), playerInfo.equippedEnchantKinds[slotIndex]);
        slotObject.insert(QStringLiteral("enchantValue"), playerInfo.equippedEnchantValues[slotIndex]);
        slotObject.insert(QStringLiteral("enhanceSuccessCount"),
                          playerInfo.equippedEnhanceSuccessCounts[slotIndex]);
        slotObject.insert(QStringLiteral("forgeSuccessCount"),
                          playerInfo.equippedForgeSuccessCounts[slotIndex]);
        slotObject.insert(QStringLiteral("enchantSuccessCount"),
                          playerInfo.equippedEnchantSuccessCounts[slotIndex]);
        array.append(slotObject);
    }
    return array;
}

void applyInventoryJournalEquipmentStateFromJson(const QJsonArray& array, Player_Information* playerInfo)
{
    if (!playerInfo) {
        return;
    }

    for (const QJsonValue& value : array) {
        const QJsonObject slotObject = value.toObject();
        const int slotIndex =
            qBound(0, jsonValueToInt(slotObject.value(QStringLiteral("slotIndex"))), MAX_EQUIPMENT_SLOT_NUM - 1);
        playerInfo->equippedItemIds[slotIndex] =
            qMax(0, jsonValueToInt(slotObject.value(QStringLiteral("itemId"))));
        playerInfo->equippedEnhanceLevels[slotIndex] =
            qMax(0, jsonValueToInt(slotObject.value(QStringLiteral("enhanceLevel"))));
        playerInfo->equippedForgeLevels[slotIndex] =
            qMax(0, jsonValueToInt(slotObject.value(QStringLiteral("forgeLevel"))));
        playerInfo->equippedEnchantKinds[slotIndex] =
            qBound(0, jsonValueToInt(slotObject.value(QStringLiteral("enchantKind"))), 4);
        playerInfo->equippedEnchantValues[slotIndex] =
            qMax(0, jsonValueToInt(slotObject.value(QStringLiteral("enchantValue"))));
        playerInfo->equippedEnhanceSuccessCounts[slotIndex] =
            qMax(playerInfo->equippedEnhanceLevels[slotIndex],
                 qMax(0, jsonValueToInt(slotObject.value(QStringLiteral("enhanceSuccessCount")))));
        playerInfo->equippedForgeSuccessCounts[slotIndex] =
            qMax(playerInfo->equippedForgeLevels[slotIndex],
                 qMax(0, jsonValueToInt(slotObject.value(QStringLiteral("forgeSuccessCount")))));
        playerInfo->equippedEnchantSuccessCounts[slotIndex] =
            qMax(0, jsonValueToInt(slotObject.value(QStringLiteral("enchantSuccessCount"))));
        if (playerInfo->equippedEnchantKinds[slotIndex] != 0) {
            playerInfo->equippedEnchantSuccessCounts[slotIndex] =
                qMax(1, playerInfo->equippedEnchantSuccessCounts[slotIndex]);
        }
    }
}

QString buildInventorySnapshotJournalPayload(const Player_Information& playerInfo,
                                             const QString& action,
                                             const QString& eventPayloadJson)
{
    QJsonObject playerObject;
    playerObject.insert(QStringLiteral("playerId"), playerInfo.player_UserId);
    playerObject.insert(QStringLiteral("playerName"), playerInfo.playerName);
    playerObject.insert(QStringLiteral("mapId"), playerInfo.mapId.trimmed().isEmpty()
                                                     ? QStringLiteral("BornWorld")
                                                     : playerInfo.mapId.trimmed());
    playerObject.insert(QStringLiteral("instanceId"), QString::number(playerInfo.instanceId));
    playerObject.insert(QStringLiteral("level"), playerInfo.level);
    playerObject.insert(QStringLiteral("experience"), QString::number(playerInfo.exp));
    playerObject.insert(QStringLiteral("health"), playerInfo.health);
    playerObject.insert(QStringLiteral("mana"), playerInfo.mana);
    playerObject.insert(QStringLiteral("attackPower"), playerInfo.attackPower);
    playerObject.insert(QStringLiteral("magicAttack"), playerInfo.magicAttack);
    playerObject.insert(QStringLiteral("independentAttack"), playerInfo.independentAttack);
    playerObject.insert(QStringLiteral("defense"), playerInfo.defense);
    playerObject.insert(QStringLiteral("magicDefense"), playerInfo.magicDefense);
    playerObject.insert(QStringLiteral("strength"), playerInfo.strength);
    playerObject.insert(QStringLiteral("intelligence"), playerInfo.intelligence);
    playerObject.insert(QStringLiteral("vitality"), playerInfo.vitality);
    playerObject.insert(QStringLiteral("spirit"), playerInfo.spirit);
    playerObject.insert(QStringLiteral("critRate"), playerInfo.critRate);
    playerObject.insert(QStringLiteral("magicCritRate"), playerInfo.magicCritRate);
    playerObject.insert(QStringLiteral("critDamage"), playerInfo.critDamage);
    playerObject.insert(QStringLiteral("attackSpeed"), playerInfo.attackSpeed);
    playerObject.insert(QStringLiteral("moveSpeed"), playerInfo.moveSpeed);
    playerObject.insert(QStringLiteral("castSpeed"), playerInfo.castSpeed);
    playerObject.insert(QStringLiteral("attackRange"), playerInfo.attackRange);
    playerObject.insert(QStringLiteral("positionX"), playerInfo.positionX);
    playerObject.insert(QStringLiteral("positionY"), playerInfo.positionY);
    playerObject.insert(QStringLiteral("direction"), playerInfo.direction);
    playerObject.insert(QStringLiteral("warehouseUnlockTier"), playerInfo.warehouseUnlockTier);
    playerObject.insert(QStringLiteral("bagEntries"), inventoryJournalItemEntriesToJson(playerInfo.bagEntries));
    playerObject.insert(QStringLiteral("warehouseEntries"),
                        inventoryJournalItemEntriesToJson(playerInfo.warehouseEntries));
    playerObject.insert(QStringLiteral("equipment"), inventoryJournalEquipmentStateToJson(playerInfo));

    QJsonObject root;
    root.insert(QStringLiteral("kind"), QStringLiteral("inventory_snapshot"));
    root.insert(QStringLiteral("action"), action.trimmed());
    root.insert(QStringLiteral("stateVersion"), static_cast<int>(playerInfo.inventoryStateVersion));
    root.insert(QStringLiteral("player"), playerObject);

    const QByteArray eventUtf8 = eventPayloadJson.trimmed().toUtf8();
    if (!eventUtf8.isEmpty()) {
        QJsonParseError error{};
        const QJsonDocument eventDocument = QJsonDocument::fromJson(eventUtf8, &error);
        if (error.error == QJsonParseError::NoError && !eventDocument.isNull()) {
            if (eventDocument.isObject()) {
                root.insert(QStringLiteral("event"), eventDocument.object());
            } else if (eventDocument.isArray()) {
                root.insert(QStringLiteral("eventItems"), eventDocument.array());
            } else {
                root.insert(QStringLiteral("eventText"), QString::fromUtf8(eventUtf8));
            }
        } else {
            root.insert(QStringLiteral("eventText"), QString::fromUtf8(eventUtf8));
        }
    }

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool parseInventorySnapshotJournalPayload(const QString& payloadJson,
                                          Player_Information* playerInfo,
                                          QString* actionOut = nullptr)
{
    if (!playerInfo) {
        return false;
    }

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(payloadJson.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("kind")).toString() != QStringLiteral("inventory_snapshot")) {
        return false;
    }

    const QJsonObject playerObject = root.value(QStringLiteral("player")).toObject();
    const int playerId = qMax(0, jsonValueToInt(playerObject.value(QStringLiteral("playerId"))));
    if (playerId <= 0) {
        return false;
    }

    Player_Information parsed(playerId,
                              playerObject.value(QStringLiteral("playerName")).toString(),
                              qMax(1, jsonValueToInt(playerObject.value(QStringLiteral("level")), 1)),
                              qMax<qint64>(0, jsonValueToLongLong(playerObject.value(QStringLiteral("experience")))),
                              qMax<qint64>(0, jsonValueToLongLong(playerObject.value(QStringLiteral("experience")))),
                              qMax(0, jsonValueToInt(playerObject.value(QStringLiteral("health")))),
                              qMax(0, jsonValueToInt(playerObject.value(QStringLiteral("mana")))),
                              jsonValueToInt(playerObject.value(QStringLiteral("attackPower"))),
                              jsonValueToInt(playerObject.value(QStringLiteral("magicAttack"))),
                              jsonValueToInt(playerObject.value(QStringLiteral("independentAttack"))),
                              jsonValueToInt(playerObject.value(QStringLiteral("defense"))),
                              jsonValueToInt(playerObject.value(QStringLiteral("magicDefense"))),
                              jsonValueToInt(playerObject.value(QStringLiteral("strength"))),
                              jsonValueToInt(playerObject.value(QStringLiteral("intelligence"))),
                              jsonValueToInt(playerObject.value(QStringLiteral("vitality"))),
                              jsonValueToInt(playerObject.value(QStringLiteral("spirit"))),
                              jsonValueToFloat(playerObject.value(QStringLiteral("critRate"))),
                              jsonValueToFloat(playerObject.value(QStringLiteral("magicCritRate"))),
                              jsonValueToFloat(playerObject.value(QStringLiteral("critDamage")), 1.5f),
                              jsonValueToFloat(playerObject.value(QStringLiteral("attackSpeed")), 1.0f),
                              jsonValueToFloat(playerObject.value(QStringLiteral("moveSpeed")), 1.0f),
                              jsonValueToFloat(playerObject.value(QStringLiteral("castSpeed")), 1.0f),
                              jsonValueToFloat(playerObject.value(QStringLiteral("attackRange"))),
                              jsonValueToFloat(playerObject.value(QStringLiteral("positionX"))),
                              jsonValueToFloat(playerObject.value(QStringLiteral("positionY"))),
                              0,
                              playerObject.value(QStringLiteral("mapId")).toString(),
                              qMax<qint64>(0, jsonValueToLongLong(playerObject.value(QStringLiteral("instanceId")))),
                              jsonValueToInt(playerObject.value(QStringLiteral("direction")), 3));

    parsed.warehouseUnlockTier =
        qMax(1, jsonValueToInt(playerObject.value(QStringLiteral("warehouseUnlockTier")), 1));
    parsed.inventoryStateVersion =
        static_cast<quint32>(qMax(0, jsonValueToInt(root.value(QStringLiteral("stateVersion")))));
    parsed.lastPersistedInventoryStateVersion = 0;
    parsed.bagEntries =
        inventoryJournalItemEntriesFromJson(playerObject.value(QStringLiteral("bagEntries")).toArray());
    parsed.warehouseEntries =
        inventoryJournalItemEntriesFromJson(playerObject.value(QStringLiteral("warehouseEntries")).toArray());
    parsed.bagEntries = normalizedPersistEntries(parsed.bagEntries);
    parsed.warehouseEntries = normalizedPersistEntries(parsed.warehouseEntries);
    parsed.bagLoaded = true;
    parsed.warehouseLoaded = true;
    parsed.authoritativeBagUpdatedAtMs = 0;
    applyInventoryJournalEquipmentStateFromJson(playerObject.value(QStringLiteral("equipment")).toArray(),
                                                &parsed);

    *playerInfo = parsed;
    if (actionOut) {
        *actionOut = root.value(QStringLiteral("action")).toString().trimmed();
    }
    return true;
}

bool appendPendingInventoryJournalEntry(CMySql& sql,
                                        int userId,
                                        quint32 stateVersion,
                                        const QString& action,
                                        const QString& payloadJson,
                                        quint64* journalId = nullptr)
{
    const bool ok = updatePreparedQuery(
        sql,
        "INSERT INTO user_inventory_journal "
        "(u_id, state_version, action_key, payload_json, status, attempt_count, last_error, applied_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "RETURNING journal_id;",
        sqlParams(userId,
                  static_cast<int>(stateVersion),
                  utf8StdString(action.trimmed()),
                  utf8StdString(payloadJson),
                  utf8StdString(QString::fromUtf8(kInventoryJournalStatusPending)),
                  0,
                  CMySql::Param::null(),
                  CMySql::Param::null()));

    if (ok && journalId) {
        *journalId = sql.LastInsertId();
    }
    return ok;
}

bool markInventoryJournalApplied(CMySql& sql, quint64 journalId, int attemptCount)
{
    return updatePreparedQuery(
        sql,
        "UPDATE user_inventory_journal "
        "SET status = ?, attempt_count = ?, last_error = ?, applied_at = CURRENT_TIMESTAMP "
        "WHERE journal_id = ?;",
        sqlParams(utf8StdString(QString::fromUtf8(kInventoryJournalStatusApplied)),
                  qMax(0, attemptCount),
                  CMySql::Param::null(),
                  static_cast<unsigned long long>(journalId)));
}

bool markInventoryJournalSuperseded(CMySql& sql, quint64 journalId)
{
    return updatePreparedQuery(
        sql,
        "UPDATE user_inventory_journal "
        "SET status = ?, last_error = ?, applied_at = COALESCE(applied_at, CURRENT_TIMESTAMP) "
        "WHERE journal_id = ?;",
        sqlParams(utf8StdString(QString::fromUtf8(kInventoryJournalStatusSuperseded)),
                  CMySql::Param::null(),
                  static_cast<unsigned long long>(journalId)));
}

bool markInventoryJournalRetryPending(quint64 journalId, int attemptCount, const QString& lastError)
{
    if (journalId == 0) {
        return false;
    }

    CMySql sql;
    return updatePreparedQuery(
        sql,
        "UPDATE user_inventory_journal "
        "SET status = ?, attempt_count = ?, last_error = ?, applied_at = ? "
        "WHERE journal_id = ?;",
        sqlParams(utf8StdString(QString::fromUtf8(kInventoryJournalStatusPending)),
                  qMax(0, attemptCount),
                  utf8StdString(lastError.left(240)),
                  CMySql::Param::null(),
                  static_cast<unsigned long long>(journalId)));
}

bool markInventoryJournalFailed(quint64 journalId, int attemptCount, const QString& lastError)
{
    if (journalId == 0) {
        return false;
    }

    CMySql sql;
    return updatePreparedQuery(
        sql,
        "UPDATE user_inventory_journal "
        "SET status = ?, attempt_count = ?, last_error = ?, applied_at = ? "
        "WHERE journal_id = ?;",
        sqlParams(utf8StdString(QString::fromUtf8(kInventoryJournalStatusFailed)),
                  qMax(0, attemptCount),
                  utf8StdString(lastError.left(240)),
                  CMySql::Param::null(),
                  static_cast<unsigned long long>(journalId)));
}

int inventoryPersistenceRetryDelayMs(int attempt)
{
    return qBound(150, 250 * qMax(1, attempt), 5000);
}

bool updateUserBasicInformation(CMySql& sql,
                                int userId,
                                int health,
                                int mana,
                                int attackPower,
                                int magicAttack,
                                int independentAttack,
                                int attackRange,
                                qlonglong experience,
                                int level,
                                int defence,
                                int magicDefence,
                                int strength,
                                int intelligence,
                                int vitality,
                                int spirit,
                                float critRate,
                                float magicCritRate,
                                float critDamage,
                                float attackSpeed,
                                float moveSpeed,
                                float castSpeed,
                                bool includePosition = false,
                                float positionX = 0.0f,
                                float positionY = 0.0f,
                                const std::array<int, MAX_EQUIPMENT_SLOT_NUM>* equippedItemIds = nullptr)
{
    QStringList assignments = {
        QStringLiteral("u_health = ?"),
        QStringLiteral("u_mana = ?"),
        QStringLiteral("u_attackpower = ?"),
        QStringLiteral("u_magicattack = ?"),
        QStringLiteral("u_independentattack = ?"),
        QStringLiteral("u_attackrange = ?"),
        QStringLiteral("u_experience = ?"),
        QStringLiteral("u_level = ?"),
        QStringLiteral("u_defence = ?"),
        QStringLiteral("u_magicdefence = ?"),
        QStringLiteral("u_strength = ?"),
        QStringLiteral("u_intelligence = ?"),
        QStringLiteral("u_vitality = ?"),
        QStringLiteral("u_spirit = ?"),
        QStringLiteral("u_critrate = ?"),
        QStringLiteral("u_magiccritrate = ?"),
        QStringLiteral("u_critdamage = ?"),
        QStringLiteral("u_attackspeed = ?"),
        QStringLiteral("u_movespeed = ?"),
        QStringLiteral("u_castspeed = ?")
    };

    std::vector<SqlParam> params = sqlParams(health,
                                             mana,
                                             attackPower,
                                             magicAttack,
                                             independentAttack,
                                             attackRange,
                                             experience,
                                             level,
                                             defence,
                                             magicDefence,
                                             strength,
                                             intelligence,
                                             vitality,
                                             spirit,
                                             critRate,
                                             magicCritRate,
                                             critDamage,
                                             attackSpeed,
                                             moveSpeed,
                                             castSpeed);

    if (includePosition) {
        assignments << QStringLiteral("u_position_x = ?")
                    << QStringLiteral("u_position_y = ?");
        params.emplace_back(positionX);
        params.emplace_back(positionY);
    }

    if (equippedItemIds) {
        assignments << QStringLiteral("u_equipment_weapon = ?")
                    << QStringLiteral("u_equipment_head = ?")
                    << QStringLiteral("u_equipment_body = ?")
                    << QStringLiteral("u_equipment_legs = ?")
                    << QStringLiteral("u_equipment_hands = ?")
                    << QStringLiteral("u_equipment_shoes = ?")
                    << QStringLiteral("u_equipment_shield = ?");
        for (int itemId : *equippedItemIds) {
            params.emplace_back(itemId);
        }
    }

    params.emplace_back(userId);
    const QString query =
        QStringLiteral("UPDATE user_basic_information SET %1 WHERE u_id = ?;")
            .arg(assignments.join(QStringLiteral(", ")));
    return updatePreparedQuery(sql, query, params);
}

bool updateUserBasicInformation(CMySql& sql,
                                const Player_Information& playerInfo,
                                bool includePosition = false,
                                bool includeEquipment = false)
{
    return updateUserBasicInformation(sql,
                                      playerInfo.player_UserId,
                                      playerInfo.health,
                                      playerInfo.mana,
                                      playerInfo.attackPower,
                                      playerInfo.magicAttack,
                                      playerInfo.independentAttack,
                                      qRound(playerInfo.attackRange),
                                      playerInfo.exp,
                                      playerInfo.level,
                                      playerInfo.defense,
                                      playerInfo.magicDefense,
                                      playerInfo.strength,
                                      playerInfo.intelligence,
                                      playerInfo.vitality,
                                      playerInfo.spirit,
                                      playerInfo.critRate,
                                      playerInfo.magicCritRate,
                                      playerInfo.critDamage,
                                      playerInfo.attackSpeed,
                                      playerInfo.moveSpeed,
                                      playerInfo.castSpeed,
                                      includePosition,
                                      playerInfo.positionX,
                                      playerInfo.positionY,
                                      includeEquipment ? &playerInfo.equippedItemIds : nullptr);
}

bool upsertWarehouseUnlockProgressState(CMySql& sql,
                                        int userId,
                                        const QString& mapId,
                                        int warehouseUnlockTier,
                                        float positionX,
                                        float positionY)
{
    return updatePreparedQuery(
        sql,
        "INSERT INTO user_progress_state "
        "(u_id, map_id, quest_step, warehouse_unlock_tier, pos_x, pos_y) "
        "VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT (u_id) DO UPDATE SET "
        "warehouse_unlock_tier = EXCLUDED.warehouse_unlock_tier, "
        "updated_at = CURRENT_TIMESTAMP;",
        sqlParams(userId,
                  utf8StdString(mapId),
                  0,
                  warehouseUnlockTier,
                  positionX,
                  positionY));
}

bool upsertPlayerProgressState(CMySql& sql,
                               int userId,
                               const QString& mapId,
                               int questStep,
                               int warehouseUnlockTier,
                               float positionX,
                               float positionY)
{
    return updatePreparedQuery(
        sql,
        "INSERT INTO user_progress_state "
        "(u_id, map_id, quest_step, warehouse_unlock_tier, pos_x, pos_y) "
        "VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT (u_id) DO UPDATE SET "
        "map_id = EXCLUDED.map_id, "
        "quest_step = EXCLUDED.quest_step, "
        "warehouse_unlock_tier = EXCLUDED.warehouse_unlock_tier, "
        "pos_x = EXCLUDED.pos_x, "
        "pos_y = EXCLUDED.pos_y, "
        "updated_at = CURRENT_TIMESTAMP;",
        sqlParams(userId,
                  utf8StdString(mapId),
                  questStep,
                  warehouseUnlockTier,
                  positionX,
                  positionY));
}

QVector<QPair<int, int>> normalizedPersistEntries(const QVector<QPair<int, int>>& rawEntries);
ItemCountMap buildItemCountMap(const QVector<QPair<int, int>>& rawEntries);
void clearTrackedEquipmentSlot(Player_Information& playerInfo, int slotIndex);
int warehouseSlotCapacityForTier(int tier);
bool persistItemTableEntries(CMySql& sql,
                             const QString& tableName,
                             int userId,
                             const QVector<QPair<int, int>>& rawEntries);
bool persistEquipmentStateBatch(CMySql& sql, int userId, const Player_Information& playerInfo);
bool persistTrackedPlayerInventoryState(CMySql& sql, const Player_Information& playerInfo);
bool persistTrackedPlayerInventoryState(const Player_Information& playerInfo);
bool persistAuthoritativeRewardState(CMySql& sql, const Player_Information& playerInfo);
bool persistAuthoritativeRewardState(const Player_Information& playerInfo);

InventoryRulesConfig loadServerInventoryRulesConfig()
{
    const QString configPath =
        JaegerShared::resolveSharedDataFilePath(QStringLiteral("inventory_rules.json"),
                                                {g_configuredSharedDataDirectory});
    return JaegerShared::loadInventoryRulesConfigFromFile(configPath);
}

const InventoryRulesConfig& serverInventoryRules()
{
    static const InventoryRulesConfig config = loadServerInventoryRulesConfig();
    return config;
}

RewardRulesConfig loadServerRewardRulesConfig()
{
    const QString configPath =
        JaegerShared::resolveSharedDataFilePath(QStringLiteral("reward_rules.json"),
                                                {g_configuredSharedDataDirectory});
    return JaegerShared::loadRewardRulesConfigFromFile(configPath);
}

const RewardRulesConfig& serverRewardRules()
{
    static const RewardRulesConfig config = loadServerRewardRulesConfig();
    return config;
}

int serverWarehouseTierCount()
{
    return qMax(1, serverInventoryRules().warehouseTierCount);
}

QHash<int, ServerItemMetadata> loadServerItemMetadataCache()
{
    const QString itemConfigPath =
        JaegerShared::resolveSharedDataFilePath(QStringLiteral("item.json"),
                                                {g_configuredSharedDataDirectory});
    return JaegerShared::loadItemMetadataFromFile(itemConfigPath);
}

const ServerItemMetadata* serverItemMetadata(int itemId)
{
    static const QHash<int, ServerItemMetadata> metadataCache = loadServerItemMetadataCache();
    const auto it = metadataCache.constFind(itemId);
    if (it == metadataCache.constEnd()) {
        return nullptr;
    }
    return &it.value();
}

QVector<QPair<int, int>> itemCountMapToEntries(const ItemCountMap& counts)
{
    QVector<QPair<int, int>> entries;
    entries.reserve(counts.size());
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        if (it.key() <= 0 || it.value() <= 0) {
            continue;
        }
        entries.push_back(qMakePair(it.key(), it.value()));
    }
    return entries;
}

int normalizedTrackedEnhanceSuccessCount(const Player_Information& playerInfo, int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= MAX_EQUIPMENT_SLOT_NUM) {
        return 0;
    }
    return qMax(qMax(0, playerInfo.equippedEnhanceSuccessCounts[slotIndex]),
                qMax(0, playerInfo.equippedEnhanceLevels[slotIndex]));
}

int normalizedTrackedForgeSuccessCount(const Player_Information& playerInfo, int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= MAX_EQUIPMENT_SLOT_NUM) {
        return 0;
    }
    return qMax(qMax(0, playerInfo.equippedForgeSuccessCounts[slotIndex]),
                qMax(0, playerInfo.equippedForgeLevels[slotIndex]));
}

int normalizedTrackedEnchantSuccessCount(const Player_Information& playerInfo, int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= MAX_EQUIPMENT_SLOT_NUM) {
        return 0;
    }
    const int legacyMinimum = playerInfo.equippedEnchantKinds[slotIndex] == 0 ? 0 : 1;
    return qMax(qMax(0, playerInfo.equippedEnchantSuccessCounts[slotIndex]), legacyMinimum);
}

int trackedBagItemCount(const Player_Information& playerInfo, int itemId)
{
    return buildItemCountMap(playerInfo.bagEntries).value(itemId, 0);
}

bool consumeTrackedBagCosts(Player_Information& playerInfo,
                            const QVector<QPair<int, int>>& costs,
                            QString* failureMessage)
{
    ItemCountMap counts = buildItemCountMap(playerInfo.bagEntries);
    QStringList missing;

    for (const auto& cost : costs) {
        const int required = qMax(0, cost.second);
        if (required <= 0) {
            continue;
        }

        const int owned = counts.value(cost.first, 0);
        if (owned < required) {
            const ServerItemMetadata* metadata = serverItemMetadata(cost.first);
            const QString itemName = (metadata && !metadata->name.isEmpty())
                                         ? metadata->name
                                         : QStringLiteral("物品%1").arg(cost.first);
            missing << QStringLiteral("%1 %2/%3").arg(itemName).arg(owned).arg(required);
        }
    }

    if (!missing.isEmpty()) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("材料不足：%1").arg(missing.join(QStringLiteral("，")));
        }
        return false;
    }

    for (const auto& cost : costs) {
        const int required = qMax(0, cost.second);
        if (required <= 0) {
            continue;
        }

        const int nextCount = counts.value(cost.first, 0) - required;
        if (nextCount > 0) {
            counts[cost.first] = nextCount;
        } else {
            counts.remove(cost.first);
        }
    }

    playerInfo.bagEntries = itemCountMapToEntries(counts);
    playerInfo.bagLoaded = true;
    playerInfo.authoritativeBagUpdatedAtMs = 0;
    return true;
}

bool addTrackedStorageItems(QVector<QPair<int, int>>& entries, int maxEntries, int itemId, int count)
{
    if (itemId <= 0 || count <= 0) {
        return false;
    }

    ItemCountMap counts = buildItemCountMap(entries);
    if (!counts.contains(itemId) && counts.size() >= maxEntries) {
        return false;
    }

    counts[itemId] += count;
    entries = itemCountMapToEntries(counts);
    return true;
}

bool consumeTrackedStorageItems(QVector<QPair<int, int>>& entries, int itemId, int count)
{
    if (itemId <= 0 || count <= 0) {
        return false;
    }

    ItemCountMap counts = buildItemCountMap(entries);
    const int owned = counts.value(itemId, 0);
    if (owned < count) {
        return false;
    }

    const int nextCount = owned - count;
    if (nextCount > 0) {
        counts[itemId] = nextCount;
    } else {
        counts.remove(itemId);
    }

    entries = itemCountMapToEntries(counts);
    return true;
}

bool serverCanEquipItemInSlot(const Player_Information& playerInfo,
                              int itemId,
                              int slotIndex,
                              QString* failureMessage)
{
    const ServerItemMetadata* metadata = serverItemMetadata(itemId);
    if (!metadata) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("目标物品不存在。");
        }
        return false;
    }

    if (slotIndex < 0 || slotIndex >= MAX_EQUIPMENT_SLOT_NUM) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("目标装备槽位无效。");
        }
        return false;
    }

    if (metadata->equipSlotIndex != slotIndex) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("%1 无法穿戴到该装备槽位。").arg(metadata->name);
        }
        return false;
    }

    if (metadata->levelRequirement > playerInfo.level) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("%1 需要角色达到 Lv.%2 才能装备。")
                                  .arg(metadata->name)
                                  .arg(metadata->levelRequirement);
        }
        return false;
    }

    return true;
}

bool moveTrackedBagItemToEquipment(Player_Information& playerInfo,
                                   int itemId,
                                   int slotIndex,
                                   QString* failureMessage)
{
    if (!serverCanEquipItemInSlot(playerInfo, itemId, slotIndex, failureMessage)) {
        return false;
    }

    if (!consumeTrackedStorageItems(playerInfo.bagEntries, itemId, 1)) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("背包中没有可穿戴的目标装备。");
        }
        return false;
    }

    const int previousEquippedItemId = playerInfo.equippedItemIds[slotIndex];
    if (previousEquippedItemId > 0
        && !addTrackedStorageItems(playerInfo.bagEntries, MAX_BAG_ITEM_NUM, previousEquippedItemId, 1))
    {
        addTrackedStorageItems(playerInfo.bagEntries, MAX_BAG_ITEM_NUM, itemId, 1);
        if (failureMessage) {
            *failureMessage = QStringLiteral("背包空间不足，无法替换当前装备。");
        }
        return false;
    }

    clearTrackedEquipmentSlot(playerInfo, slotIndex);
    playerInfo.equippedItemIds[slotIndex] = itemId;
    playerInfo.bagEntries = normalizedPersistEntries(playerInfo.bagEntries);
    playerInfo.bagLoaded = true;
    playerInfo.authoritativeBagUpdatedAtMs = 0;
    return true;
}

bool moveTrackedEquipmentToBag(Player_Information& playerInfo,
                               int slotIndex,
                               QString* failureMessage)
{
    if (slotIndex < 0 || slotIndex >= MAX_EQUIPMENT_SLOT_NUM) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("目标装备槽位无效。");
        }
        return false;
    }

    const int equippedItemId = playerInfo.equippedItemIds[slotIndex];
    if (equippedItemId <= 0) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("该槽位当前没有可卸下的装备。");
        }
        return false;
    }

    if (!addTrackedStorageItems(playerInfo.bagEntries, MAX_BAG_ITEM_NUM, equippedItemId, 1)) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("背包空间不足，无法卸下该装备。");
        }
        return false;
    }

    clearTrackedEquipmentSlot(playerInfo, slotIndex);
    playerInfo.bagEntries = normalizedPersistEntries(playerInfo.bagEntries);
    playerInfo.bagLoaded = true;
    playerInfo.authoritativeBagUpdatedAtMs = 0;
    return true;
}

bool moveTrackedEquipmentBetweenSlots(Player_Information& playerInfo,
                                      int fromSlot,
                                      int toSlot,
                                      QString* failureMessage)
{
    if (fromSlot < 0 || fromSlot >= MAX_EQUIPMENT_SLOT_NUM || toSlot < 0 || toSlot >= MAX_EQUIPMENT_SLOT_NUM) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("目标装备槽位无效。");
        }
        return false;
    }

    if (fromSlot == toSlot) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("源槽位与目标槽位相同。");
        }
        return false;
    }

    const int fromItemId = playerInfo.equippedItemIds[fromSlot];
    const int toItemId = playerInfo.equippedItemIds[toSlot];
    if (fromItemId <= 0) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("源装备槽位当前为空。");
        }
        return false;
    }

    if (!serverCanEquipItemInSlot(playerInfo, fromItemId, toSlot, failureMessage)) {
        return false;
    }
    if (toItemId > 0 && !serverCanEquipItemInSlot(playerInfo, toItemId, fromSlot, failureMessage)) {
        return false;
    }

    std::swap(playerInfo.equippedItemIds[fromSlot], playerInfo.equippedItemIds[toSlot]);
    std::swap(playerInfo.equippedEnhanceLevels[fromSlot], playerInfo.equippedEnhanceLevels[toSlot]);
    std::swap(playerInfo.equippedForgeLevels[fromSlot], playerInfo.equippedForgeLevels[toSlot]);
    std::swap(playerInfo.equippedEnchantKinds[fromSlot], playerInfo.equippedEnchantKinds[toSlot]);
    std::swap(playerInfo.equippedEnchantValues[fromSlot], playerInfo.equippedEnchantValues[toSlot]);
    std::swap(playerInfo.equippedEnhanceSuccessCounts[fromSlot],
              playerInfo.equippedEnhanceSuccessCounts[toSlot]);
    std::swap(playerInfo.equippedForgeSuccessCounts[fromSlot],
              playerInfo.equippedForgeSuccessCounts[toSlot]);
    std::swap(playerInfo.equippedEnchantSuccessCounts[fromSlot],
              playerInfo.equippedEnchantSuccessCounts[toSlot]);
    return true;
}

bool moveTrackedBagItemsToWarehouse(Player_Information& playerInfo,
                                    int itemId,
                                    int transferCount,
                                    QString* failureMessage)
{
    const int ownedCount = trackedBagItemCount(playerInfo, itemId);
    if (ownedCount <= 0) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("背包中没有可存入仓库的目标物品。");
        }
        return false;
    }

    const int normalizedTransferCount = qBound(0, transferCount, ownedCount);
    if (normalizedTransferCount == 0) {
        return true;
    }

    const int warehouseCapacity = warehouseSlotCapacityForTier(playerInfo.warehouseUnlockTier);
    if (!buildItemCountMap(playerInfo.warehouseEntries).contains(itemId)
        && buildItemCountMap(playerInfo.warehouseEntries).size() >= warehouseCapacity)
    {
        if (failureMessage) {
            *failureMessage = QStringLiteral("仓库空间不足，无法继续存入该物品。");
        }
        return false;
    }

    if (!consumeTrackedStorageItems(playerInfo.bagEntries, itemId, normalizedTransferCount)
        || !addTrackedStorageItems(playerInfo.warehouseEntries,
                                   warehouseCapacity,
                                   itemId,
                                   normalizedTransferCount))
    {
        if (failureMessage) {
            *failureMessage = QStringLiteral("仓库存放失败，请稍后重试。");
        }
        return false;
    }

    playerInfo.bagEntries = normalizedPersistEntries(playerInfo.bagEntries);
    playerInfo.warehouseEntries = normalizedPersistEntries(playerInfo.warehouseEntries);
    playerInfo.bagLoaded = true;
    playerInfo.warehouseLoaded = true;
    playerInfo.authoritativeBagUpdatedAtMs = 0;
    return true;
}

bool moveTrackedWarehouseItemsToBag(Player_Information& playerInfo,
                                    int itemId,
                                    int transferCount,
                                    QString* failureMessage)
{
    ItemCountMap warehouseCounts = buildItemCountMap(playerInfo.warehouseEntries);
    const int ownedCount = warehouseCounts.value(itemId, 0);
    if (ownedCount <= 0) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("仓库中没有可取回的目标物品。");
        }
        return false;
    }

    const int normalizedTransferCount = qBound(0, transferCount, ownedCount);
    if (normalizedTransferCount == 0) {
        return true;
    }

    if (!buildItemCountMap(playerInfo.bagEntries).contains(itemId)
        && buildItemCountMap(playerInfo.bagEntries).size() >= MAX_BAG_ITEM_NUM)
    {
        if (failureMessage) {
            *failureMessage = QStringLiteral("背包空间不足，无法从仓库取回该物品。");
        }
        return false;
    }

    if (!consumeTrackedStorageItems(playerInfo.warehouseEntries, itemId, normalizedTransferCount)
        || !addTrackedStorageItems(playerInfo.bagEntries,
                                   MAX_BAG_ITEM_NUM,
                                   itemId,
                                   normalizedTransferCount))
    {
        if (failureMessage) {
            *failureMessage = QStringLiteral("取回物品失败，请稍后重试。");
        }
        return false;
    }

    playerInfo.bagEntries = normalizedPersistEntries(playerInfo.bagEntries);
    playerInfo.warehouseEntries = normalizedPersistEntries(playerInfo.warehouseEntries);
    playerInfo.bagLoaded = true;
    playerInfo.warehouseLoaded = true;
    playerInfo.authoritativeBagUpdatedAtMs = 0;
    return true;
}

int serverStrengthenSuccessRate(const Player_Information& playerInfo, int slotIndex)
{
    return JaegerShared::craftSuccessRateFor(serverInventoryRules().strengthen,
                                             normalizedTrackedEnhanceSuccessCount(playerInfo, slotIndex));
}

int serverForgeSuccessRate(const Player_Information& playerInfo, int slotIndex)
{
    return JaegerShared::craftSuccessRateFor(serverInventoryRules().forge,
                                             normalizedTrackedForgeSuccessCount(playerInfo, slotIndex));
}

int serverEnchantSuccessRate(const Player_Information& playerInfo, int slotIndex)
{
    return JaegerShared::craftSuccessRateFor(serverInventoryRules().enchant,
                                             normalizedTrackedEnchantSuccessCount(playerInfo, slotIndex));
}

QVector<QPair<int, int>> serverWarehouseUnlockCosts(int nextTier)
{
    return serverInventoryRules().warehouseCostsByTier.value(qBound(1, nextTier, serverWarehouseTierCount()));
}

QVector<QPair<int, int>> serverStrengthenCosts(const Player_Information& playerInfo, int slotIndex)
{
    const int nextLevel = qMax(1, qMax(0, playerInfo.equippedEnhanceLevels[slotIndex]) + 1);
    return JaegerShared::buildFormulaCosts(serverInventoryRules().strengthen.costs, nextLevel);
}

QVector<QPair<int, int>> serverForgeCosts(const Player_Information& playerInfo, int slotIndex)
{
    const int nextLevel = qMax(1, qMax(0, playerInfo.equippedForgeLevels[slotIndex]) + 1);
    return JaegerShared::buildFormulaCosts(serverInventoryRules().forge.costs, nextLevel);
}

QVector<QPair<int, int>> serverEnchantCosts(const Player_Information& playerInfo, int slotIndex)
{
    const int successCount = normalizedTrackedEnchantSuccessCount(playerInfo, slotIndex);
    return JaegerShared::buildFormulaCosts(serverInventoryRules().enchant.costs, successCount);
}

int serverEquipmentMaxHealthBonus(const Player_Information& playerInfo)
{
    int totalBonus = 0;
    for (int slotIndex = 0; slotIndex < MAX_EQUIPMENT_SLOT_NUM; ++slotIndex) {
        const ServerItemMetadata* metadata = serverItemMetadata(playerInfo.equippedItemIds[slotIndex]);
        if (!metadata) {
            continue;
        }

        totalBonus += qMax(0, metadata->hpBonus);
        if (metadata->kind == ServerItemKind::Armor) {
            totalBonus += qMax(0, playerInfo.equippedForgeLevels[slotIndex]) * 12;
            if (playerInfo.equippedEnchantKinds[slotIndex] == kEnchantKindVitality) {
                totalBonus += qMax(0, playerInfo.equippedEnchantValues[slotIndex]);
            }
        }
    }
    return totalBonus;
}

int serverEquipmentMaxManaBonus(const Player_Information& playerInfo)
{
    int totalBonus = 0;
    for (int slotIndex = 0; slotIndex < MAX_EQUIPMENT_SLOT_NUM; ++slotIndex) {
        const ServerItemMetadata* metadata = serverItemMetadata(playerInfo.equippedItemIds[slotIndex]);
        if (!metadata) {
            continue;
        }

        totalBonus += qMax(0, metadata->mpBonus);
        if (metadata->kind == ServerItemKind::Armor) {
            totalBonus += qMax(0, playerInfo.equippedForgeLevels[slotIndex]) * 6;
            if (playerInfo.equippedEnchantKinds[slotIndex] == kEnchantKindVitality) {
                totalBonus += qRound(qMax(0, playerInfo.equippedEnchantValues[slotIndex]) * 0.5f);
            }
        }
    }
    return totalBonus;
}

void clampTrackedVitalsToEquipmentCaps(Player_Information& playerInfo)
{
    const CombatBalance::PlayerStats baseline = CombatBalance::playerStats(CombatBalance::clampLevel(playerInfo.level));
    const int maxHealth = baseline.maxHealth + serverEquipmentMaxHealthBonus(playerInfo);
    const int maxMana = baseline.maxMana + serverEquipmentMaxManaBonus(playerInfo);
    playerInfo.health = qBound(0, playerInfo.health, qMax(1, maxHealth));
    playerInfo.mana = qBound(0, playerInfo.mana, qMax(0, maxMana));
}

bool persistTrackedPlayerInventoryState(CMySql& sql, const Player_Information& playerInfo)
{
    QString safeMapId = playerInfo.mapId.trimmed().isEmpty()
                            ? QStringLiteral("BornWorld")
                            : playerInfo.mapId.trimmed();
    if (!updateUserBasicInformation(sql, playerInfo, true, true)) {
        return false;
    }

    if (!persistEquipmentStateBatch(sql, playerInfo.player_UserId, playerInfo)) {
        return false;
    }
    if (!persistItemTableEntries(sql, QStringLiteral("user_item"), playerInfo.player_UserId, playerInfo.bagEntries)) {
        return false;
    }
    if (!persistItemTableEntries(sql,
                                 QStringLiteral("user_warehouse_item"),
                                 playerInfo.player_UserId,
                                 playerInfo.warehouseEntries))
    {
        return false;
    }

    return upsertWarehouseUnlockProgressState(sql,
                                              playerInfo.player_UserId,
                                              safeMapId,
                                              qBound(1, playerInfo.warehouseUnlockTier, serverWarehouseTierCount()),
                                              playerInfo.positionX,
                                              playerInfo.positionY);
}

[[maybe_unused]] bool persistTrackedPlayerInventoryState(const Player_Information& playerInfo)
{
    CMySql sql;
    if (!sql.BeginTransaction()) {
        return false;
    }
    const bool ok = persistTrackedPlayerInventoryState(sql, playerInfo);
    if (!ok) {
        sql.RollbackTransaction();
        return false;
    }
    if (!sql.CommitTransaction()) {
        sql.RollbackTransaction();
        return false;
    }
    return true;
}

int fillInventoryDeltaEntries(InventoryItemDeltaEntry* output,
                              int maxCount,
                              const QVector<QPair<int, int>>& previousEntries,
                              const QVector<QPair<int, int>>& currentEntries)
{
    if (!output || maxCount <= 0) {
        return 0;
    }

    const ItemCountMap beforeCounts = buildItemCountMap(previousEntries);
    const ItemCountMap afterCounts = buildItemCountMap(currentEntries);
    QSet<int> itemIds;
    for (auto it = beforeCounts.constBegin(); it != beforeCounts.constEnd(); ++it) {
        itemIds.insert(it.key());
    }
    for (auto it = afterCounts.constBegin(); it != afterCounts.constEnd(); ++it) {
        itemIds.insert(it.key());
    }

    QList<int> orderedItemIds = itemIds.values();
    std::sort(orderedItemIds.begin(), orderedItemIds.end());

    int count = 0;
    for (int itemId : orderedItemIds) {
        if (count >= maxCount) {
            break;
        }

        const int delta = afterCounts.value(itemId, 0) - beforeCounts.value(itemId, 0);
        if (itemId <= 0 || delta == 0) {
            continue;
        }

        output[count].itemId = itemId;
        output[count].delta = delta;
        ++count;
    }
    return count;
}

int fillEquipmentChangeEntries(InventoryEquipmentChangeEntry* output,
                               int maxCount,
                               const Player_Information& previousInfo,
                               const Player_Information& currentInfo)
{
    if (!output || maxCount <= 0) {
        return 0;
    }

    int count = 0;
    for (int slotIndex = 0; slotIndex < MAX_EQUIPMENT_SLOT_NUM && count < maxCount; ++slotIndex) {
        const bool changed =
            previousInfo.equippedItemIds[slotIndex] != currentInfo.equippedItemIds[slotIndex]
            || previousInfo.equippedEnhanceLevels[slotIndex] != currentInfo.equippedEnhanceLevels[slotIndex]
            || previousInfo.equippedForgeLevels[slotIndex] != currentInfo.equippedForgeLevels[slotIndex]
            || previousInfo.equippedEnchantKinds[slotIndex] != currentInfo.equippedEnchantKinds[slotIndex]
            || previousInfo.equippedEnchantValues[slotIndex] != currentInfo.equippedEnchantValues[slotIndex]
            || previousInfo.equippedEnhanceSuccessCounts[slotIndex]
                   != currentInfo.equippedEnhanceSuccessCounts[slotIndex]
            || previousInfo.equippedForgeSuccessCounts[slotIndex]
                   != currentInfo.equippedForgeSuccessCounts[slotIndex]
            || previousInfo.equippedEnchantSuccessCounts[slotIndex]
                   != currentInfo.equippedEnchantSuccessCounts[slotIndex];
        if (!changed) {
            continue;
        }

        InventoryEquipmentChangeEntry& entry = output[count];
        entry.slotIndex = slotIndex;
        entry.itemId = currentInfo.equippedItemIds[slotIndex];
        entry.enhanceLevel = currentInfo.equippedEnhanceLevels[slotIndex];
        entry.forgeLevel = currentInfo.equippedForgeLevels[slotIndex];
        entry.enchantKind = currentInfo.equippedEnchantKinds[slotIndex];
        entry.enchantValue = currentInfo.equippedEnchantValues[slotIndex];
        entry.enhanceSuccessCount = currentInfo.equippedEnhanceSuccessCounts[slotIndex];
        entry.forgeSuccessCount = currentInfo.equippedForgeSuccessCounts[slotIndex];
        entry.enchantSuccessCount = currentInfo.equippedEnchantSuccessCounts[slotIndex];
        ++count;
    }
    return count;
}

QString inventoryActionName(quint16 action)
{
    switch (action) {
    case _inventory_action_use_consumable: return QStringLiteral("use_consumable");
    case _inventory_action_unlock_warehouse: return QStringLiteral("unlock_warehouse");
    case _inventory_action_strengthen: return QStringLiteral("strengthen");
    case _inventory_action_forge: return QStringLiteral("forge");
    case _inventory_action_enchant: return QStringLiteral("enchant");
    case _inventory_action_equip_from_bag: return QStringLiteral("equip_from_bag");
    case _inventory_action_unequip_to_bag: return QStringLiteral("unequip_to_bag");
    case _inventory_action_move_equipment: return QStringLiteral("move_equipment");
    case _inventory_action_move_bag_to_warehouse: return QStringLiteral("move_bag_to_warehouse");
    case _inventory_action_move_warehouse_to_bag: return QStringLiteral("move_warehouse_to_bag");
    default: return QStringLiteral("unknown");
    }
}

QString inventoryActionJournalPayload(const STRU_INVENTORY_ACTION_RS& rs)
{
    QJsonObject root;
    root.insert(QStringLiteral("playerId"), rs.playerId);
    root.insert(QStringLiteral("action"), inventoryActionName(rs.action));
    root.insert(QStringLiteral("actionCode"), static_cast<int>(rs.action));
    root.insert(QStringLiteral("actionArg"), static_cast<int>(rs.actionArg));
    root.insert(QStringLiteral("requestItemId"), rs.requestItemId);
    root.insert(QStringLiteral("requestItemCount"), rs.requestItemCount);
    root.insert(QStringLiteral("requestSlotIndex"), rs.requestSlotIndex);
    root.insert(QStringLiteral("result"), rs.result);
    root.insert(QStringLiteral("stateVersion"), static_cast<int>(rs.stateVersion));
    root.insert(QStringLiteral("warehouseUnlockTier"), rs.warehouseUnlockTier);
    root.insert(QStringLiteral("level"), rs.level);
    root.insert(QStringLiteral("experience"), static_cast<qint64>(rs.experience));
    root.insert(QStringLiteral("health"), rs.health);
    root.insert(QStringLiteral("mana"), rs.mana);
    root.insert(QStringLiteral("message"), QString::fromUtf8(rs.message).trimmed());

    QJsonArray bagDeltaArray;
    for (int i = 0; i < qBound(0, static_cast<int>(rs.bagDeltaCount), MAX_BAG_ITEM_NUM); ++i) {
        QJsonObject deltaObject;
        deltaObject.insert(QStringLiteral("itemId"), rs.bagDeltas[i].itemId);
        deltaObject.insert(QStringLiteral("delta"), rs.bagDeltas[i].delta);
        bagDeltaArray.append(deltaObject);
    }
    root.insert(QStringLiteral("bagDeltas"), bagDeltaArray);

    QJsonArray warehouseDeltaArray;
    for (int i = 0; i < qBound(0, static_cast<int>(rs.warehouseDeltaCount), MAX_WAREHOUSE_ITEM_NUM); ++i) {
        QJsonObject deltaObject;
        deltaObject.insert(QStringLiteral("itemId"), rs.warehouseDeltas[i].itemId);
        deltaObject.insert(QStringLiteral("delta"), rs.warehouseDeltas[i].delta);
        warehouseDeltaArray.append(deltaObject);
    }
    root.insert(QStringLiteral("warehouseDeltas"), warehouseDeltaArray);

    QJsonArray equipmentArray;
    for (int i = 0; i < qBound(0, static_cast<int>(rs.equipmentChangeCount), MAX_EQUIPMENT_SLOT_NUM); ++i) {
        const InventoryEquipmentChangeEntry& entry = rs.equipmentChanges[i];
        QJsonObject equipmentObject;
        equipmentObject.insert(QStringLiteral("slotIndex"), entry.slotIndex);
        equipmentObject.insert(QStringLiteral("itemId"), entry.itemId);
        equipmentObject.insert(QStringLiteral("enhanceLevel"), entry.enhanceLevel);
        equipmentObject.insert(QStringLiteral("forgeLevel"), entry.forgeLevel);
        equipmentObject.insert(QStringLiteral("enchantKind"), entry.enchantKind);
        equipmentObject.insert(QStringLiteral("enchantValue"), entry.enchantValue);
        equipmentObject.insert(QStringLiteral("enhanceSuccessCount"), entry.enhanceSuccessCount);
        equipmentObject.insert(QStringLiteral("forgeSuccessCount"), entry.forgeSuccessCount);
        equipmentObject.insert(QStringLiteral("enchantSuccessCount"), entry.enchantSuccessCount);
        equipmentArray.append(equipmentObject);
    }
    root.insert(QStringLiteral("equipmentChanges"), equipmentArray);

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void fillInventoryActionDeltaResponse(STRU_INVENTORY_ACTION_RS& rs,
                                      const Player_Information& previousInfo,
                                      const Player_Information& currentInfo,
                                      const STRU_INVENTORY_ACTION_RQ& rq,
                                      int result,
                                      const QString& message,
                                      quint16 responseFlags)
{
    memset(&rs, 0, sizeof(rs));
    rs.playerId = currentInfo.player_UserId;
    rs.action = rq.action;
    rs.actionArg = rq.actionArg;
    rs.result = result;
    rs.stateVersion = currentInfo.inventoryStateVersion;
    rs.requestItemId = rq.itemId;
    rs.requestItemCount = rq.itemCount;
    rs.requestSlotIndex = rq.slotIndex;
    rs.level = currentInfo.level;
    rs.experience = currentInfo.exp;
    rs.health = currentInfo.health;
    rs.mana = currentInfo.mana;
    rs.warehouseUnlockTier = qBound(1, currentInfo.warehouseUnlockTier, serverWarehouseTierCount());
    rs.responseFlags = responseFlags;
    rs.bagDeltaCount = static_cast<quint16>(fillInventoryDeltaEntries(rs.bagDeltas,
                                                                      MAX_BAG_ITEM_NUM,
                                                                      previousInfo.bagEntries,
                                                                      currentInfo.bagEntries));
    rs.warehouseDeltaCount = static_cast<quint16>(fillInventoryDeltaEntries(rs.warehouseDeltas,
                                                                            MAX_WAREHOUSE_ITEM_NUM,
                                                                            previousInfo.warehouseEntries,
                                                                            currentInfo.warehouseEntries));
    rs.equipmentChangeCount = static_cast<quint16>(fillEquipmentChangeEntries(rs.equipmentChanges,
                                                                              MAX_EQUIPMENT_SLOT_NUM,
                                                                              previousInfo,
                                                                              currentInfo));
    std::strncpy(rs.message, message.toUtf8().constData(), sizeof(rs.message) - 1);
}

[[maybe_unused]] bool appendInventoryJournalEntry(CMySql& sql,
                                                  int userId,
                                                  quint32 stateVersion,
                                                  const QString& action,
                                                  const QString& payloadJson)
{
    return updatePreparedQuery(
        sql,
        "INSERT INTO user_inventory_journal (u_id, state_version, action_key, payload_json) "
        "VALUES (?, ?, ?, ?);",
        sqlParams(userId,
                  static_cast<int>(stateVersion),
                  utf8StdString(action),
                  utf8StdString(payloadJson)));
}

QString readObjectProperty(XMLElement* objectElement, const QString& propertyName)
{
    if (!objectElement) {
        return {};
    }

    XMLElement* propertiesElement = objectElement->FirstChildElement("properties");
    for (XMLElement* prop = propertiesElement ? propertiesElement->FirstChildElement("property") : nullptr;
         prop;
         prop = prop->NextSiblingElement("property"))
    {
        const QString currentName = prop->Attribute("name") ? QString::fromUtf8(prop->Attribute("name")) : QString();
        if (currentName.compare(propertyName, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const char* value = prop->Attribute("value");
        if (value) {
            return QString::fromUtf8(value);
        }
        if (prop->GetText()) {
            return QString::fromUtf8(prop->GetText());
        }
        return {};
    }
    return {};
}

QString readMapProperty(XMLElement* mapElement, const QString& propertyName)
{
    if (!mapElement) {
        return {};
    }

    XMLElement* propertiesElement = mapElement->FirstChildElement("properties");
    for (XMLElement* prop = propertiesElement ? propertiesElement->FirstChildElement("property") : nullptr;
         prop;
         prop = prop->NextSiblingElement("property"))
    {
        const QString currentName = prop->Attribute("name") ? QString::fromUtf8(prop->Attribute("name")) : QString();
        if (currentName.compare(propertyName, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const char* value = prop->Attribute("value");
        if (value) {
            return QString::fromUtf8(value);
        }
        if (prop->GetText()) {
            return QString::fromUtf8(prop->GetText());
        }
        return {};
    }
    return {};
}

int readMapIntProperty(XMLElement* mapElement, const QString& propertyName, int fallback)
{
    bool ok = false;
    const int parsed = readMapProperty(mapElement, propertyName).toInt(&ok);
    return ok ? parsed : fallback;
}

int readIntProperty(XMLElement* objectElement, const QString& propertyName, int fallback)
{
    bool ok = false;
    const int parsed = readObjectProperty(objectElement, propertyName).toInt(&ok);
    return ok ? parsed : fallback;
}

float readFloatProperty(XMLElement* objectElement, const QString& propertyName, float fallback)
{
    bool ok = false;
    const float parsed = readObjectProperty(objectElement, propertyName).toFloat(&ok);
    return ok ? parsed : fallback;
}

bool readBoolProperty(XMLElement* objectElement, const QString& propertyName, bool fallback)
{
    const QString value = readObjectProperty(objectElement, propertyName).trimmed().toLower();
    if (value.isEmpty()) {
        return fallback;
    }
    if (value == QStringLiteral("1")
        || value == QStringLiteral("true")
        || value == QStringLiteral("yes")
        || value == QStringLiteral("on"))
    {
        return true;
    }
    if (value == QStringLiteral("0")
        || value == QStringLiteral("false")
        || value == QStringLiteral("no")
        || value == QStringLiteral("off"))
    {
        return false;
    }
    return fallback;
}

QString normalizeMonsterName(const QString& rawName, int runtimeId)
{
    const QString trimmed = rawName.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("怪物%1").arg(runtimeId) : trimmed;
}

float serverPlayerDodgeChance(int level, float moveSpeedBonus, int spirit)
{
    const int lv = qBound(1, level, 100);
    return qBound(0.01f,
                  0.02f + static_cast<float>(lv) * 0.00022f
                      + qMax(0.0f, moveSpeedBonus) * 0.16f
                      + static_cast<float>(qMax(0, spirit)) * 0.00007f,
                  0.24f);
}

float serverMonsterHitChance(int level, CombatBalance::MonsterTier tier)
{
    const int lv = qBound(1, level, 100);
    float base = 0.88f + static_cast<float>(lv) * 0.00025f;
    if (tier == CombatBalance::MonsterTier::Elite) {
        base += 0.02f;
    } else if (tier == CombatBalance::MonsterTier::Boss) {
        base += 0.04f;
    }
    return qBound(0.70f, base, 0.995f);
}

qreal serverDefenceReductionRatio(int defenceValue)
{
    const qreal safeDefence = qMax(0, defenceValue);
    return qBound<qreal>(0.0, safeDefence / (safeDefence + 920.0), 0.78);
}

QString normalizedSkillId(const char* rawSkillId)
{
    return QString::fromUtf8(rawSkillId ? rawSkillId : "").trimmed().toLower();
}

QString hashPasswordForStorage(const QString& plainTextPassword)
{
    const QByteArray digest = QCryptographicHash::hash(plainTextPassword.toUtf8(),
                                                       QCryptographicHash::Sha256);
    return QStringLiteral("sha256$%1").arg(QString::fromLatin1(digest.toHex()));
}

bool isStoredPasswordHashed(const QString& storedPassword)
{
    return storedPassword.startsWith(QStringLiteral("sha256$"), Qt::CaseInsensitive);
}

bool verifyStoredPassword(const QString& plainTextPassword, const QString& storedPassword)
{
    const QString normalizedStoredPassword = storedPassword.trimmed();
    if (isStoredPasswordHashed(normalizedStoredPassword)) {
        return normalizedStoredPassword == hashPasswordForStorage(plainTextPassword);
    }
    return normalizedStoredPassword == plainTextPassword;
}

QString buildSkillHitWindowKey(int attackerId, const QString& skillId)
{
    return QStringLiteral("%1#%2").arg(attackerId).arg(skillId.trimmed().toLower());
}

int syncedSkillLevel(const Player_Information& playerInfo, const QString& skillId, int fallbackLevel)
{
    const auto it = playerInfo.skillLevels.constFind(skillId.trimmed().toLower());
    if (it == playerInfo.skillLevels.constEnd()) {
        return qMax(0, fallbackLevel);
    }
    return qMax(0, it.value());
}

qreal syncedPassivePhysicalBonus(const Player_Information& playerInfo)
{
    return qMax(0, syncedSkillLevel(playerInfo, QStringLiteral("weapon_mastery"), 0)) * 0.055;
}

qreal syncedPassiveMagicalBonus(const Player_Information& playerInfo)
{
    return qMax(0, syncedSkillLevel(playerInfo, QStringLiteral("mana_resonance"), 0)) * 0.060;
}

/**
 * @brief 处理serverRollChance相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool serverRollChance(double chance)
{
    const double safeChance = qBound(0.0, chance, 1.0);
    return QRandomGenerator::global()->generateDouble() < safeChance;
}

/**
 * @brief 处理serverRandomInclusive相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
int serverRandomInclusive(int minValue, int maxValue)
{
    const int lower = qMin(minValue, maxValue);
    const int upper = qMax(minValue, maxValue);
    if (lower == upper) {
        return lower;
    }
    return QRandomGenerator::global()->bounded(upper - lower + 1) + lower;
}

/**
 * @brief 处理applyServerExperienceReward相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
int applyServerExperienceReward(Player_Information& playerInfo, int expReward, bool* leveledUp)
{
    if (leveledUp) {
        *leveledUp = false;
    }

    const int safeReward = qMax(1, expReward);
    long long carriedExp = qMax(0LL, playerInfo.exp) + safeReward;
    const int oldLevel = CombatBalance::clampLevel(playerInfo.level);
    int newLevel = oldLevel;

    while (newLevel < 100) {
        const long long needExp = CombatBalance::playerStats(newLevel).expToNext;
        if (carriedExp < needExp) {
            break;
        }
        carriedExp -= needExp;
        ++newLevel;
    }

    playerInfo.exp = carriedExp;
    if (newLevel <= oldLevel) {
        return safeReward;
    }

    const CombatBalance::PlayerStats oldStats = CombatBalance::playerStats(oldLevel);
    const CombatBalance::PlayerStats newStats = CombatBalance::playerStats(newLevel);
    playerInfo.level = newLevel;
    playerInfo.health = qMax(1, playerInfo.health + (newStats.maxHealth - oldStats.maxHealth));
    playerInfo.mana = qMax(0, playerInfo.mana + (newStats.maxMana - oldStats.maxMana));
    playerInfo.attackPower = qMax(0, playerInfo.attackPower + (newStats.attack - oldStats.attack));
    playerInfo.magicAttack = qMax(0, playerInfo.magicAttack + (newStats.magicAttack - oldStats.magicAttack));
    playerInfo.independentAttack = qMax(0, playerInfo.independentAttack + (newStats.independentAttack - oldStats.independentAttack));
    playerInfo.defense = qMax(0, playerInfo.defense + (newStats.defence - oldStats.defence));
    playerInfo.magicDefense = qMax(0, playerInfo.magicDefense + (newStats.magicDefence - oldStats.magicDefence));
    playerInfo.strength = qMax(0, playerInfo.strength + (newStats.strength - oldStats.strength));
    playerInfo.intelligence = qMax(0, playerInfo.intelligence + (newStats.intelligence - oldStats.intelligence));
    playerInfo.vitality = qMax(0, playerInfo.vitality + (newStats.vitality - oldStats.vitality));
    playerInfo.spirit = qMax(0, playerInfo.spirit + (newStats.spirit - oldStats.spirit));
    playerInfo.critRate = qMax(0.0f, playerInfo.critRate + (newStats.critRate - oldStats.critRate));
    playerInfo.magicCritRate = qMax(0.0f, playerInfo.magicCritRate + (newStats.magicCritRate - oldStats.magicCritRate));
    playerInfo.critDamage = qMax(1.0f, playerInfo.critDamage + (newStats.critDamage - oldStats.critDamage));
    playerInfo.attackSpeed = qMax(0.0f, playerInfo.attackSpeed + (newStats.attackSpeed - oldStats.attackSpeed));
    playerInfo.moveSpeed = qMax(0.0f, playerInfo.moveSpeed + (newStats.moveSpeed - oldStats.moveSpeed));
    playerInfo.castSpeed = qMax(0.0f, playerInfo.castSpeed + (newStats.castSpeed - oldStats.castSpeed));
    playerInfo.attackRange = qMax(1.0f, playerInfo.attackRange + (newStats.attackRange - oldStats.attackRange));

    if (leveledUp) {
        *leveledUp = true;
    }
    return safeReward;
}

/**
 * @brief 处理serverSetItemPoolByLevel相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
QVector<int> serverSetItemPoolByLevel(int setLevel)
{
    switch (setLevel) {
    case 1:
        return {1, 12, 10, 13, 14, 15, 7};
    case 10:
        return {16, 17, 18, 19, 20, 21, 22};
    case 20:
        return {23, 24, 25, 26, 27, 28, 29};
    case 30:
        return {30, 31, 32, 33, 34, 35, 36};
    case 40:
        return {37, 38, 39, 40, 41, 42, 43};
    case 50:
        return {44, 45, 46, 47, 48, 49, 50};
    case 60:
        return {51, 52, 53, 54, 55, 56, 57};
    default:
        break;
    }
    return {};
}

/**
 * @brief 处理serverResolveDropSetLevel相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
int serverResolveDropSetLevel(int monsterLevel)
{
    const int lv = qMax(1, monsterLevel);
    if (lv >= 60) return 60;
    if (lv >= 50) return 50;
    if (lv >= 40) return 40;
    if (lv >= 30) return 30;
    if (lv >= 20) return 20;
    if (lv >= 10) return 10;
    return 1;
}

/**
 * @brief 处理serverGenerateMonsterDrops相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
QVector<QPair<int, int>> serverGenerateMonsterDrops(const MonsterSpawnDefinition& definition)
{
    const int monsterLevel = qMax(1, definition.monsterLevel);
    const CombatBalance::MonsterTier tier = definition.monsterTier;

    double tierFactor = 1.0;
    double materialBoost = 1.0;
    double gearChance = 0.08;
    switch (tier) {
    case CombatBalance::MonsterTier::Elite:
        tierFactor = 1.75;
        materialBoost = 1.55;
        gearChance = 0.24;
        break;
    case CombatBalance::MonsterTier::Boss:
        tierFactor = 2.80;
        materialBoost = 2.35;
        gearChance = 0.48;
        break;
    case CombatBalance::MonsterTier::Normal:
        break;
    }

    QHash<int, int> merged;
    auto addDrop = [&merged](int itemId, int count) {
        if (itemId <= 0 || count <= 0) {
            return;
        }
        merged[itemId] += count;
    };

    const float goldRate = qMax(0.20f, definition.dropGoldRate);
    const float materialRate = qMax(0.20f, definition.dropMaterialRate);
    const float equipmentRate = qMax(0.0f, definition.dropEquipmentRate);

    const int goldMin = qMax(12, qRound((26.0 + monsterLevel * 16.0) * tierFactor * goldRate));
    const int goldMax = qMax(goldMin, qRound(goldMin * 1.48));
    addDrop(1001, serverRandomInclusive(goldMin, goldMax));

    if (serverRollChance(qMin(0.98, 0.88 * materialRate))) {
        const int colorlessCount = qMax(1, qRound((1.0 + monsterLevel / 11.0) * materialBoost * materialRate));
        addDrop(1005, colorlessCount);
    }
    if (serverRollChance(qMin(0.98, (tier == CombatBalance::MonsterTier::Boss ? 0.95
                                                                               : (tier == CombatBalance::MonsterTier::Elite ? 0.72 : 0.52))
                                   * materialRate)))
    {
        addDrop(1002, serverRandomInclusive(1, qMax(1, qRound(materialBoost * materialRate * 2.6))));
    }
    if (serverRollChance(qMin(0.95, (tier == CombatBalance::MonsterTier::Boss ? 0.86
                                                                               : (tier == CombatBalance::MonsterTier::Elite ? 0.48 : 0.20))
                                   * materialRate)))
    {
        addDrop(1003, serverRandomInclusive(1, qMax(1, qRound(materialBoost * materialRate * 1.7))));
    }
    if (serverRollChance(qMin(0.92, (tier == CombatBalance::MonsterTier::Boss ? 0.76
                                                                               : (tier == CombatBalance::MonsterTier::Elite ? 0.30 : 0.10))
                                   * materialRate)))
    {
        addDrop(1004, serverRandomInclusive(1, qMax(1, qRound(materialBoost * materialRate * 1.4))));
    }
    if (serverRollChance(qMin(0.96, (tier == CombatBalance::MonsterTier::Boss ? 0.90
                                                                               : (tier == CombatBalance::MonsterTier::Elite ? 0.56 : 0.18))
                                   * materialRate)))
    {
        addDrop(1006, serverRandomInclusive(1, qMax(1, qRound(materialBoost * materialRate * 1.8))));
    }

    if (monsterLevel >= 50 && serverRollChance(0.20 + (tier == CombatBalance::MonsterTier::Boss ? 0.22 : 0.0))) {
        addDrop(59, 1);
    } else if (monsterLevel >= 30 && serverRollChance(0.32)) {
        addDrop(58, 1);
    } else if (serverRollChance(0.25)) {
        addDrop(11, 1);
    }

    gearChance *= equipmentRate;
    const int setLevel = definition.dropSetLevel > 0
                             ? serverResolveDropSetLevel(definition.dropSetLevel)
                             : serverResolveDropSetLevel(monsterLevel);
    const QVector<int> basePool = serverSetItemPoolByLevel(setLevel);
    if (!basePool.isEmpty() && serverRollChance(gearChance)) {
        addDrop(basePool[QRandomGenerator::global()->bounded(basePool.size())], 1);
    }

    if (tier == CombatBalance::MonsterTier::Boss) {
        int bonusSetLevel = setLevel;
        if (serverRollChance(0.18)) {
            bonusSetLevel = qMin(60, setLevel + 10);
        }
        const QVector<int> bonusPool = serverSetItemPoolByLevel(bonusSetLevel);
        if (!bonusPool.isEmpty() && serverRollChance(0.28)) {
            addDrop(bonusPool[QRandomGenerator::global()->bounded(bonusPool.size())], 1);
        }
    }

QVector<QPair<int, int>> drops;
    drops.reserve(merged.size());
    for (auto it = merged.constBegin(); it != merged.constEnd(); ++it) {
        drops.push_back(qMakePair(it.key(), it.value()));
    }
    return drops;
}

/**
 * @brief 处理serverGenerateDungeonSettlementDrops相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
QVector<QPair<int, int>> serverGenerateDungeonSettlementDrops(const QString& mapId,
                                                              int recommendedLevel,
                                                              int totalMonsterCount,
                                                              int memberCount)
{
    Q_UNUSED(mapId);

    const int safeLevel = qMax(1, recommendedLevel);
    const int safeMonsterCount = qMax(1, totalMonsterCount);
    const int safeMemberCount = qBound(1, memberCount, MAX_PARTY_MEMBER_NUM);

    QHash<int, int> merged;
    auto addDrop = [&merged](int itemId, int count) {
        if (itemId <= 0 || count <= 0) {
            return;
        }
        merged[itemId] += count;
    };

    const double memberFactor = 1.0 + (safeMemberCount - 1) * 0.10;
    addDrop(1001, qMax(80, qRound((safeLevel * 34.0 + safeMonsterCount * 22.0) * memberFactor)));
    addDrop(1005, qMax(2, qRound((safeLevel / 7.0 + safeMonsterCount * 0.55) * memberFactor)));
    addDrop(1002, qMax(1, qRound(safeLevel / 18.0 + safeMonsterCount * 0.20)));
    addDrop(1006, qMax(1, qRound(0.8 + safeMonsterCount * 0.16)));

    if (safeLevel >= 18 && serverRollChance(qMin(0.78, 0.28 + safeMonsterCount * 0.035))) {
        addDrop(1003, qMax(1, qRound(1.0 + safeMemberCount * 0.4)));
    }
    if (safeLevel >= 24 && serverRollChance(qMin(0.62, 0.18 + safeMonsterCount * 0.025))) {
        addDrop(1004, 1);
    }

    const int setLevel = serverResolveDropSetLevel(safeLevel);
    const QVector<int> basePool = serverSetItemPoolByLevel(setLevel);
    if (!basePool.isEmpty() && serverRollChance(0.26 + qMin(0.24, safeMonsterCount * 0.03))) {
        addDrop(basePool[QRandomGenerator::global()->bounded(basePool.size())], 1);
    }

    const int bonusSetLevel = qMin(60, setLevel + 10);
    const QVector<int> bonusPool = serverSetItemPoolByLevel(bonusSetLevel);
    if (!bonusPool.isEmpty() && safeMonsterCount >= 4 && serverRollChance(0.10 + qMin(0.16, safeMemberCount * 0.04))) {
        addDrop(bonusPool[QRandomGenerator::global()->bounded(bonusPool.size())], 1);
    }

    QVector<QPair<int, int>> drops;
    drops.reserve(merged.size());
    for (auto it = merged.constBegin(); it != merged.constEnd(); ++it) {
        drops.push_back(qMakePair(it.key(), it.value()));
    }
    return drops;
}

/**
 * @brief 判断地图是否为持续刷怪场
 * @author Jaeger
 * @date 2025.3.28
 */
bool isContinuousRespawnMap(const QString& mapId)
{
    return mapId.compare(QStringLiteral("MonsterField"), Qt::CaseInsensitive) == 0;
}

constexpr qint64 kDungeonEmptyRoomPersistMs = 10 * 60 * 1000;

/**
 * @brief 处理applyServerMonsterRewardProgression相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
int applyServerMonsterRewardProgression(Player_Information& playerInfo,
                                        const QString& mapId,
                                        int monsterLevel,
                                        CombatBalance::MonsterTier tier,
                                        bool* leveledUp)
{
    if (leveledUp) {
        *leveledUp = false;
    }

    const CombatBalance::MonsterStats monsterStats = CombatBalance::monsterStats(qMax(1, monsterLevel), tier);
    int expReward = qMax(1, monsterStats.expReward);
    if (mapId.compare(QStringLiteral("MonsterField"), Qt::CaseInsensitive) == 0) {
        expReward = qMax(1, qRound(expReward * 1.10));
    }
    return applyServerExperienceReward(playerInfo, expReward, leveledUp);
}

/**
 * @brief 处理bagEntriesFromSaveRequest相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
[[maybe_unused]] QVector<QPair<int, int>> bagEntriesFromSaveRequest(const STRU_SAVE_RQ* rq)
{
    Q_UNUSED(rq);
    return {};
}

/**
 * @brief 处理warehouseEntriesFromSaveRequest相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
[[maybe_unused]] QVector<QPair<int, int>> warehouseEntriesFromSaveRequest(const STRU_SAVE_RQ* rq)
{
    Q_UNUSED(rq);
    return {};
}

QVector<QPair<int, int>> normalizedPersistEntries(const QVector<QPair<int, int>>& rawEntries)
{
    QMap<int, int> mergedEntries;
    for (const auto& entry : rawEntries) {
        if (entry.first <= 0 || entry.second <= 0) {
            continue;
        }
        mergedEntries[entry.first] += entry.second;
    }

    QVector<QPair<int, int>> normalizedEntries;
    normalizedEntries.reserve(mergedEntries.size());
    for (auto it = mergedEntries.constBegin(); it != mergedEntries.constEnd(); ++it) {
        normalizedEntries.push_back(qMakePair(it.key(), it.value()));
    }
    return normalizedEntries;
}

using EquipmentIntArray = std::array<int, MAX_EQUIPMENT_SLOT_NUM>;

int warehouseSlotCapacityForTier(int tier)
{
    const int tierCount = serverWarehouseTierCount();
    const int tierSlotCount = qMax(1, MAX_WAREHOUSE_ITEM_NUM / tierCount);
    return qBound(tierSlotCount,
                  qBound(1, tier, tierCount) * tierSlotCount,
                  MAX_WAREHOUSE_ITEM_NUM);
}

[[maybe_unused]] EquipmentIntArray requestedEquippedItemIdsFromSaveRequest(const STRU_SAVE_RQ* rq)
{
    EquipmentIntArray ids{};
    Q_UNUSED(rq);
    return ids;
}

ItemCountMap buildItemCountMap(const QVector<QPair<int, int>>& rawEntries)
{
    ItemCountMap counts;
    const QVector<QPair<int, int>> normalizedEntries = normalizedPersistEntries(rawEntries);
    for (const auto& entry : normalizedEntries) {
        if (entry.first <= 0 || entry.second <= 0) {
            continue;
        }
        counts[entry.first] += entry.second;
    }
    return counts;
}

void addEquippedItemsToCountMap(ItemCountMap& counts, const EquipmentIntArray& equippedItemIds)
{
    for (int itemId : equippedItemIds) {
        if (itemId <= 0) {
            continue;
        }
        counts[itemId] += 1;
    }
}

QVector<QPair<int, int>> bagEntriesExcludingEquipped(const QVector<QPair<int, int>>& rawBagEntries,
                                                     const EquipmentIntArray& equippedItemIds)
{
    ItemCountMap counts = buildItemCountMap(rawBagEntries);
    for (int itemId : equippedItemIds) {
        if (itemId <= 0) {
            continue;
        }

        const int current = counts.value(itemId, 0);
        if (current <= 1) {
            counts.remove(itemId);
        } else {
            counts[itemId] = current - 1;
        }
    }
    return itemCountMapToEntries(counts);
}

[[maybe_unused]] bool inventoryCountsMatch(const QVector<QPair<int, int>>& authoritativeBagEntries,
                                           const QVector<QPair<int, int>>& authoritativeWarehouseEntries,
                                           const EquipmentIntArray& authoritativeEquippedItemIds,
                                           const QVector<QPair<int, int>>& requestedBagEntries,
                                           const QVector<QPair<int, int>>& requestedWarehouseEntries,
                                           const EquipmentIntArray& requestedEquippedItemIds)
{
    ItemCountMap authoritativeCounts = buildItemCountMap(authoritativeBagEntries);
    ItemCountMap requestedCounts = buildItemCountMap(requestedBagEntries);
    const ItemCountMap authoritativeWarehouseCounts = buildItemCountMap(authoritativeWarehouseEntries);
    const ItemCountMap requestedWarehouseCounts = buildItemCountMap(requestedWarehouseEntries);

    for (auto it = authoritativeWarehouseCounts.constBegin(); it != authoritativeWarehouseCounts.constEnd(); ++it) {
        authoritativeCounts[it.key()] += it.value();
    }
    for (auto it = requestedWarehouseCounts.constBegin(); it != requestedWarehouseCounts.constEnd(); ++it) {
        requestedCounts[it.key()] += it.value();
    }

    addEquippedItemsToCountMap(authoritativeCounts, authoritativeEquippedItemIds);
    addEquippedItemsToCountMap(requestedCounts, requestedEquippedItemIds);
    return authoritativeCounts == requestedCounts;
}

void clearTrackedEquipmentSlot(Player_Information& playerInfo, int slotIndex)
{
    playerInfo.equippedItemIds[slotIndex] = 0;
    playerInfo.equippedEnhanceLevels[slotIndex] = 0;
    playerInfo.equippedForgeLevels[slotIndex] = 0;
    playerInfo.equippedEnchantKinds[slotIndex] = 0;
    playerInfo.equippedEnchantValues[slotIndex] = 0;
    playerInfo.equippedEnhanceSuccessCounts[slotIndex] = 0;
    playerInfo.equippedForgeSuccessCounts[slotIndex] = 0;
    playerInfo.equippedEnchantSuccessCounts[slotIndex] = 0;
}

[[maybe_unused]] void adoptRequestedEquipmentLayout(Player_Information& playerInfo, const EquipmentIntArray& requestedEquippedItemIds)
{
    const EquipmentIntArray previousItemIds = playerInfo.equippedItemIds;
    const EquipmentIntArray previousEnhanceLevels = playerInfo.equippedEnhanceLevels;
    const EquipmentIntArray previousForgeLevels = playerInfo.equippedForgeLevels;
    const EquipmentIntArray previousEnchantKinds = playerInfo.equippedEnchantKinds;
    const EquipmentIntArray previousEnchantValues = playerInfo.equippedEnchantValues;
    const EquipmentIntArray previousEnhanceSuccessCounts = playerInfo.equippedEnhanceSuccessCounts;
    const EquipmentIntArray previousForgeSuccessCounts = playerInfo.equippedForgeSuccessCounts;
    const EquipmentIntArray previousEnchantSuccessCounts = playerInfo.equippedEnchantSuccessCounts;
    std::array<bool, MAX_EQUIPMENT_SLOT_NUM> consumedPreviousSlots{};

    for (int slotIndex = 0; slotIndex < MAX_EQUIPMENT_SLOT_NUM; ++slotIndex) {
        const int requestedItemId = requestedEquippedItemIds[slotIndex];
        int sourceSlotIndex = -1;

        if (requestedItemId > 0) {
            if (previousItemIds[slotIndex] == requestedItemId && !consumedPreviousSlots[slotIndex]) {
                sourceSlotIndex = slotIndex;
            } else {
                for (int previousSlotIndex = 0; previousSlotIndex < MAX_EQUIPMENT_SLOT_NUM; ++previousSlotIndex) {
                    if (consumedPreviousSlots[previousSlotIndex]) {
                        continue;
                    }
                    if (previousItemIds[previousSlotIndex] != requestedItemId) {
                        continue;
                    }
                    sourceSlotIndex = previousSlotIndex;
                    break;
                }
            }
        }

        if (sourceSlotIndex < 0) {
            clearTrackedEquipmentSlot(playerInfo, slotIndex);
            playerInfo.equippedItemIds[slotIndex] = requestedItemId;
            continue;
        }

        consumedPreviousSlots[sourceSlotIndex] = true;
        playerInfo.equippedItemIds[slotIndex] = requestedItemId;
        playerInfo.equippedEnhanceLevels[slotIndex] = previousEnhanceLevels[sourceSlotIndex];
        playerInfo.equippedForgeLevels[slotIndex] = previousForgeLevels[sourceSlotIndex];
        playerInfo.equippedEnchantKinds[slotIndex] = previousEnchantKinds[sourceSlotIndex];
        playerInfo.equippedEnchantValues[slotIndex] = previousEnchantValues[sourceSlotIndex];
        playerInfo.equippedEnhanceSuccessCounts[slotIndex] = previousEnhanceSuccessCounts[sourceSlotIndex];
        playerInfo.equippedForgeSuccessCounts[slotIndex] = previousForgeSuccessCounts[sourceSlotIndex];
        playerInfo.equippedEnchantSuccessCounts[slotIndex] = previousEnchantSuccessCounts[sourceSlotIndex];
    }
}

bool persistItemTableEntries(CMySql& sql,
                             const QString& tableName,
                             int userId,
                             const QVector<QPair<int, int>>& rawEntries)
{
    const QVector<QPair<int, int>> nextEntries = normalizedPersistEntries(rawEntries);
    std::list<std::string> storedRows;
    const QString selectQuery =
        QStringLiteral("SELECT item_id, item_count FROM %1 WHERE u_id = ?;").arg(tableName);
    if (!selectPreparedQuery(sql, selectQuery, sqlParams(userId), 2, storedRows)) {
        return false;
    }

    ItemCountMap currentCounts;
    while (storedRows.size() >= 2) {
        const std::string itemIdText = storedRows.front();
        storedRows.pop_front();
        const std::string itemCountText = storedRows.front();
        storedRows.pop_front();

        try {
            const int itemId = std::stoi(itemIdText);
            const int itemCount = std::stoi(itemCountText);
            if (itemId > 0 && itemCount > 0) {
                currentCounts[itemId] = itemCount;
            }
        } catch (...) {
        }
    }

    const ItemCountMap nextCounts = buildItemCountMap(nextEntries);

    const QString deleteQuery =
        QStringLiteral("DELETE FROM %1 WHERE u_id = ? AND item_id = ?;").arg(tableName);
    for (auto it = currentCounts.constBegin(); it != currentCounts.constEnd(); ++it) {
        if (nextCounts.contains(it.key())) {
            continue;
        }

        if (!updatePreparedQuery(sql, deleteQuery, sqlParams(userId, it.key()))) {
            return false;
        }
    }

    const QString upsertQuery =
        QStringLiteral("INSERT INTO %1 (u_id, item_id, item_count) VALUES (?, ?, ?) "
                       "ON CONFLICT (u_id, item_id) DO UPDATE SET item_count = EXCLUDED.item_count;")
            .arg(tableName);
    for (auto it = nextCounts.constBegin(); it != nextCounts.constEnd(); ++it) {
        if (currentCounts.value(it.key(), 0) == it.value()) {
            continue;
        }

        if (!updatePreparedQuery(sql,
                                 upsertQuery,
                                 sqlParams(userId, it.key(), it.value()))) {
            return false;
        }
    }

    return true;
}

bool persistEquipmentStateBatch(CMySql& sql, int userId, const Player_Information& playerInfo)
{
    for (int slotIndex = 0; slotIndex < MAX_EQUIPMENT_SLOT_NUM; ++slotIndex) {
        if (!updatePreparedQuery(
                sql,
                "INSERT INTO user_equipment_state "
                "(u_id, slot_index, item_id, enhance_level, forge_level, enchant_kind, enchant_value, "
                "enhance_success_count, forge_success_count, enchant_success_count) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                "ON CONFLICT (u_id, slot_index) DO UPDATE SET "
                "item_id = EXCLUDED.item_id, "
                "enhance_level = EXCLUDED.enhance_level, "
                "forge_level = EXCLUDED.forge_level, "
                "enchant_kind = EXCLUDED.enchant_kind, "
                "enchant_value = EXCLUDED.enchant_value, "
                "enhance_success_count = EXCLUDED.enhance_success_count, "
                "forge_success_count = EXCLUDED.forge_success_count, "
                "enchant_success_count = EXCLUDED.enchant_success_count;",
                sqlParams(userId,
                          slotIndex,
                          playerInfo.equippedItemIds[slotIndex],
                          playerInfo.equippedEnhanceLevels[slotIndex],
                          playerInfo.equippedForgeLevels[slotIndex],
                          playerInfo.equippedEnchantKinds[slotIndex],
                          playerInfo.equippedEnchantValues[slotIndex],
                          playerInfo.equippedEnhanceSuccessCounts[slotIndex],
                          playerInfo.equippedForgeSuccessCounts[slotIndex],
                          playerInfo.equippedEnchantSuccessCounts[slotIndex]))) {
            return false;
        }
    }

    return true;
}

/**
 * @brief 处理applyDropsToTrackedBag相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
QVector<QPair<int, int>> applyDropsToTrackedBag(Player_Information& playerInfo,
                                                const QVector<QPair<int, int>>& plannedDrops,
                                                qint64 nowMs)
{
    QVector<QPair<int, int>> acceptedDrops;
    if (!playerInfo.bagLoaded) {
        playerInfo.bagEntries.clear();
        playerInfo.bagLoaded = true;
    }

    for (const auto& drop : plannedDrops) {
        const int itemId = drop.first;
        const int itemCount = drop.second;
        if (itemId <= 0 || itemCount <= 0) {
            continue;
        }

        bool merged = false;
        for (auto& entry : playerInfo.bagEntries) {
            if (entry.first != itemId) {
                continue;
            }
            entry.second += itemCount;
            merged = true;
            acceptedDrops.push_back(qMakePair(itemId, itemCount));
            break;
        }

        if (merged) {
            continue;
        }

        if (playerInfo.bagEntries.size() >= MAX_BAG_ITEM_NUM) {
            continue;
        }

        playerInfo.bagEntries.push_back(qMakePair(itemId, itemCount));
        acceptedDrops.push_back(qMakePair(itemId, itemCount));
    }

    if (!acceptedDrops.isEmpty()) {
        playerInfo.authoritativeBagUpdatedAtMs = nowMs;
    }
    return acceptedDrops;
}

/**
 * @brief 处理persistAuthoritativeRewardState相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool persistAuthoritativeRewardState(CMySql& sql, const Player_Information& playerInfo)
{
    if (!updateUserBasicInformation(sql, playerInfo)) {
        return false;
    }
    return persistItemTableEntries(sql,
                                   QStringLiteral("user_item"),
                                   playerInfo.player_UserId,
                                   playerInfo.bagEntries);
}

[[maybe_unused]] bool persistAuthoritativeRewardState(const Player_Information& playerInfo)
{
    CMySql sql;
    if (!sql.BeginTransaction()) {
        return false;
    }
    const bool ok = persistAuthoritativeRewardState(sql, playerInfo);
    if (!ok) {
        sql.RollbackTransaction();
        return false;
    }
    if (!sql.CommitTransaction()) {
        sql.RollbackTransaction();
        return false;
    }
    return true;
}

struct ServerSkillDamageProfile
{
    bool magical = false;
    qreal damageMultiplier = 1.0;
    int flatDamage = 0;
    qreal comboBonusMultiplier = 0.0;
    bool persistent = false;
    qreal persistentMultiplierScale = 1.0;
    qreal persistentFlatScale = 1.0;
    bool known = false;
};

QPointF playerCenterPoint(const Player_Information& playerInfo)
{
    return QPointF(playerInfo.positionX + kServerPlayerSize / 2.0,
                   playerInfo.positionY + kServerPlayerSize / 2.0);
}

QRectF buildDirectionalImpactRect(const Player_Information& playerInfo, qreal range, qreal width)
{
    const qreal px = playerInfo.positionX;
    const qreal py = playerInfo.positionY;
    const qreal centerX = px + kServerPlayerSize / 2.0;
    const qreal centerY = py + kServerPlayerSize / 2.0;
    const qreal safeRange = qMax<qreal>(30.0, range);
    const qreal safeWidth = qMax<qreal>(28.0, width);

    switch (playerInfo.direction) {
    case n:
        return QRectF(centerX - safeWidth / 2.0, py - safeRange, safeWidth, safeRange + kServerPlayerSize / 2.0);
    case s:
        return QRectF(centerX - safeWidth / 2.0, py + kServerPlayerSize / 2.0, safeWidth, safeRange + kServerPlayerSize / 2.0);
    case w:
        return QRectF(px - safeRange, centerY - safeWidth / 2.0, safeRange + kServerPlayerSize / 2.0, safeWidth);
    case e:
        return QRectF(px + kServerPlayerSize / 2.0, centerY - safeWidth / 2.0, safeRange + kServerPlayerSize / 2.0, safeWidth);
    case nw:
        return QRectF(px - safeRange * 0.86, py - safeRange * 0.86, safeRange * 0.86 + safeWidth, safeRange * 0.86 + safeWidth);
    case ne:
        return QRectF(px + kServerPlayerSize / 2.0, py - safeRange * 0.86, safeRange * 0.86 + safeWidth / 2.0, safeRange * 0.86 + safeWidth);
    case sw:
        return QRectF(px - safeRange * 0.86, py + kServerPlayerSize / 2.0, safeRange * 0.86 + safeWidth / 2.0, safeRange * 0.86 + safeWidth);
    case se:
        return QRectF(px + kServerPlayerSize / 2.0, py + kServerPlayerSize / 2.0, safeRange * 0.86 + safeWidth / 2.0, safeRange * 0.86 + safeWidth);
    default:
        break;
    }

    return QRectF(px, py, kServerPlayerSize, kServerPlayerSize);
}

QRectF buildCircularImpactRect(const Player_Information& playerInfo, qreal range, qreal radius)
{
    const QPointF center = playerCenterPoint(playerInfo);
    qreal offsetX = 0.0;
    qreal offsetY = 0.0;
    switch (playerInfo.direction) {
    case n: offsetY = -range; break;
    case s: offsetY = range; break;
    case w: offsetX = -range; break;
    case e: offsetX = range; break;
    case nw: offsetX = -range * 0.78; offsetY = -range * 0.78; break;
    case ne: offsetX = range * 0.78; offsetY = -range * 0.78; break;
    case sw: offsetX = -range * 0.78; offsetY = range * 0.78; break;
    case se: offsetX = range * 0.78; offsetY = range * 0.78; break;
    default: break;
    }
    return QRectF(center.x() + offsetX - radius,
                  center.y() + offsetY - radius,
                  radius * 2.0,
                  radius * 2.0);
}

QRectF buildBasicAttackRect(const Player_Information& playerInfo)
{
    qreal attackX = playerInfo.positionX;
    qreal attackY = playerInfo.positionY;
    qreal attackWidth = kServerPlayerSize;
    qreal attackHeight = kServerPlayerSize;
    const qreal range = qBound<qreal>(24.0, playerInfo.attackRange, 180.0);

    switch (playerInfo.direction) {
    case n:
        attackY -= range + kServerPlayerSize / 2.0;
        attackX -= kServerPlayerSize / 2.0;
        attackWidth = kServerPlayerSize * 2.0;
        attackHeight = 10.0;
        break;
    case ne:
        attackX += kServerPlayerSize;
        attackY -= range;
        attackWidth = range;
        attackHeight = range;
        break;
    case nw:
        attackX -= range;
        attackY -= range;
        attackWidth = range;
        attackHeight = range;
        break;
    case s:
        attackY += range + kServerPlayerSize / 2.0 + kServerPlayerSize - 10.0;
        attackX -= kServerPlayerSize / 2.0;
        attackWidth = kServerPlayerSize * 2.0;
        attackHeight = 10.0;
        break;
    case sw:
        attackX -= range;
        attackY += kServerPlayerSize;
        attackWidth = range;
        attackHeight = range;
        break;
    case se:
        attackX += kServerPlayerSize;
        attackY += kServerPlayerSize;
        attackWidth = range;
        attackHeight = range;
        break;
    case w:
        attackY -= kServerPlayerSize / 2.0;
        attackX -= range + kServerPlayerSize / 2.0;
        attackWidth = 10.0;
        attackHeight = kServerPlayerSize * 2.0;
        break;
    case e:
        attackY -= kServerPlayerSize / 2.0;
        attackX += range + kServerPlayerSize / 2.0 + kServerPlayerSize - 10.0;
        attackWidth = 10.0;
        attackHeight = kServerPlayerSize * 2.0;
        break;
    default:
        break;
    }

    return QRectF(attackX, attackY, attackWidth, attackHeight);
}

QRectF buildServerSkillImpactRect(const Player_Information& playerInfo, const QString& skillId)
{
    const QString normalizedSkill = skillId.trimmed().toLower();
    if (normalizedSkill == QStringLiteral("blade_arc")) {
        return buildDirectionalImpactRect(playerInfo, 82.0, 54.0);
    }
    if (normalizedSkill == QStringLiteral("break_through")) {
        return buildDirectionalImpactRect(playerInfo, 138.0, 58.0);
    }
    if (normalizedSkill == QStringLiteral("arcane_ring")) {
        return buildCircularImpactRect(playerInfo, 104.0, 64.0);
    }
    if (normalizedSkill == QStringLiteral("meteor_crash")) {
        return buildCircularImpactRect(playerInfo, 128.0, 78.0);
    }
    if (normalizedSkill == QStringLiteral("wavefield_aura")) {
        const QPointF center = playerCenterPoint(playerInfo);
        const qreal fieldRadius = qMax<qreal>(74.0, 60.0 * 1.35);
        return QRectF(center.x() - fieldRadius,
                      center.y() - fieldRadius,
                      fieldRadius * 2.0,
                      fieldRadius * 2.0);
    }
    return {};
}

ServerSkillDamageProfile resolveServerSkillDamageProfile(const Player_Information& playerInfo,
                                                         const QString& skillId,
                                                         quint16 hitFlags)
{
    const QString normalizedSkill = skillId.trimmed().toLower();
    ServerSkillDamageProfile profile;
    profile.magical = (hitFlags & _monster_hit_flag_magical) != 0;
    int skillLevel = qMax(1, syncedSkillLevel(playerInfo, normalizedSkill, normalizedSkill.isEmpty() ? 0 : 1));

    if (normalizedSkill == QStringLiteral("blade_arc")) {
        profile.known = true;
        profile.magical = false;
        profile.damageMultiplier = 1.22 + qMax(0, skillLevel - 1) * 0.11;
        profile.flatDamage = 16 + qMax(0, skillLevel - 1) * 8;
        profile.comboBonusMultiplier = 0.18;
    } else if (normalizedSkill == QStringLiteral("break_through")) {
        profile.known = true;
        profile.magical = false;
        profile.damageMultiplier = 1.68 + qMax(0, skillLevel - 1) * 0.16;
        profile.flatDamage = 24 + qMax(0, skillLevel - 1) * 12;
        profile.comboBonusMultiplier = 0.28;
    } else if (normalizedSkill == QStringLiteral("arcane_ring")) {
        profile.known = true;
        profile.magical = true;
        profile.damageMultiplier = 1.84 + qMax(0, skillLevel - 1) * 0.14;
        profile.flatDamage = 28 + qMax(0, skillLevel - 1) * 14;
        profile.comboBonusMultiplier = 0.16;
    } else if (normalizedSkill == QStringLiteral("meteor_crash")) {
        profile.known = true;
        profile.magical = true;
        profile.damageMultiplier = 2.36 + qMax(0, skillLevel - 1) * 0.20;
        profile.flatDamage = 38 + qMax(0, skillLevel - 1) * 18;
        profile.comboBonusMultiplier = 0.30;
    } else if (normalizedSkill == QStringLiteral("wavefield_aura")) {
        profile.known = true;
        profile.magical = false;
        profile.damageMultiplier = 1.95 + qMax(0, skillLevel - 1) * 0.15;
        profile.flatDamage = 30 + qMax(0, skillLevel - 1) * 12;
        profile.comboBonusMultiplier = 0.20;
        profile.persistent = true;
        profile.persistentMultiplierScale = (hitFlags & _monster_hit_flag_combo_ready) != 0 ? 0.40 : 0.35;
        profile.persistentFlatScale = (hitFlags & _monster_hit_flag_combo_ready) != 0 ? 0.58 : 0.50;
    }

    if ((hitFlags & _monster_hit_flag_combo_ready) != 0) {
        profile.damageMultiplier *= (1.0 + qMax<qreal>(0.0, profile.comboBonusMultiplier));
    }

    if (profile.persistent && (hitFlags & _monster_hit_flag_persistent) != 0) {
        profile.damageMultiplier *= profile.persistentMultiplierScale;
        profile.flatDamage = qMax(1, qRound(profile.flatDamage * profile.persistentFlatScale));
    }

    return profile;
}

int computeServerBasicAttackDamage(const Player_Information& attackerInfo,
                                   int monsterLevel,
                                   CombatBalance::MonsterTier tier)
{
    const float hitChance = qBound(0.40f,
                                   CombatBalance::playerHitChance(attackerInfo.level,
                                                                  attackerInfo.strength,
                                                                  attackerInfo.intelligence)
                                       - CombatBalance::monsterDodgeChance(monsterLevel, tier),
                                   0.995f);
    if (QRandomGenerator::global()->generateDouble() > hitChance) {
        return 0;
    }

    const bool isCritical = QRandomGenerator::global()->generateDouble() < attackerInfo.critRate;
    qreal baseDamage = attackerInfo.attackPower
                       + attackerInfo.independentAttack * 0.82
                       + attackerInfo.strength * 1.18
                       + (attackerInfo.strength + attackerInfo.intelligence) * 0.08;
    baseDamage *= (1.0 + syncedPassivePhysicalBonus(attackerInfo) * 0.35);
    const CombatBalance::MonsterStats targetStats = CombatBalance::monsterStats(qMax(1, monsterLevel), tier);
    const qreal reducedDamage = baseDamage * (1.0 - CombatBalance::defenceReductionRatio(targetStats.defence));
    const qreal finalDamage = isCritical ? reducedDamage * attackerInfo.critDamage : reducedDamage;
    return qMax(1, qRound(finalDamage));
}

int computeServerSkillDamage(const Player_Information& attackerInfo,
                             const QString& skillId,
                             quint16 hitFlags,
                             int monsterLevel,
                             CombatBalance::MonsterTier tier)
{
    const ServerSkillDamageProfile profile = resolveServerSkillDamageProfile(attackerInfo, skillId, hitFlags);
    const float hitChance = qBound(0.42f,
                                   CombatBalance::playerHitChance(attackerInfo.level,
                                                                  attackerInfo.strength,
                                                                  attackerInfo.intelligence)
                                       - CombatBalance::monsterDodgeChance(monsterLevel, tier) * 0.85f,
                                   0.997f);
    if (QRandomGenerator::global()->generateDouble() > hitChance) {
        return 0;
    }

    const qreal critRate = profile.magical ? attackerInfo.magicCritRate : attackerInfo.critRate;
    const bool isCritical = QRandomGenerator::global()->generateDouble() < critRate;
    qreal baseDamage = 0.0;
    if (profile.magical) {
        baseDamage = attackerInfo.magicAttack
                     + attackerInfo.independentAttack * 0.96
                     + attackerInfo.intelligence * 1.22
                     + attackerInfo.strength * 0.12;
        baseDamage *= (1.0 + syncedPassiveMagicalBonus(attackerInfo));
    } else {
        baseDamage = attackerInfo.attackPower
                     + attackerInfo.independentAttack * 0.90
                     + attackerInfo.strength * 1.28
                     + attackerInfo.intelligence * 0.10;
        baseDamage *= (1.0 + syncedPassivePhysicalBonus(attackerInfo));
    }

    if (profile.known) {
        baseDamage *= (1.0 + qBound(0.0f, attackerInfo.castSpeed, 0.45f) * 0.22);
    }

    const CombatBalance::MonsterStats targetStats = CombatBalance::monsterStats(qMax(1, monsterLevel), tier);
    const int targetDefence = profile.magical ? targetStats.magicDefence : targetStats.defence;
    const qreal scaledDamage = (baseDamage * qMax<qreal>(0.1, profile.damageMultiplier) + profile.flatDamage)
                               * (1.0 - CombatBalance::defenceReductionRatio(targetDefence));
    const qreal finalDamage = isCritical ? scaledDamage * attackerInfo.critDamage : scaledDamage;
    return qMax(1, qRound(finalDamage));
}
}

core *core::m_pcore = nullptr;
/**
 * @brief 构造core对象并完成基础初始化
 * @author Jaeger
 * @date 2025.3.28
 */
core::core(QObject* parent)
    : ICore(parent)
{
    JaegerDebug();
    m_pTCPNet = new TCPNet(this);
    m_pKcpNet = new KcpNet(this);

    // 设置TCPNet的KcpNet指针
    m_pTCPNet->setKcpNet(m_pKcpNet);
    connect(m_pTCPNet, &TCPNet::clientDisconnected,
            this, &core::handleClientDisconnected);
    // 连接KCP数据接收信号到dealData
    connect(m_pKcpNet, &KcpNet::kcpDataReceived,
            this, [this](quint64 clientId, const QByteArray& data) {
                // KCP数据直接交给dealData处理
                dealData(clientId, data);
            });

    m_dungeonTickTimer = new QTimer(this);
    m_dungeonTickTimer->setInterval(120);
    connect(m_dungeonTickTimer, &QTimer::timeout, this, &core::tickDungeonRooms);

    m_inventoryPersistTimer = new QTimer(this);
    m_inventoryPersistTimer->setSingleShot(true);
    connect(m_inventoryPersistTimer, &QTimer::timeout, this, &core::processInventoryPersistenceQueue);

}

/**
 * @brief 应用运行时配置并更新监听与共享数据目录
 * @author Jaeger
 * @date 2025.3.28
 */
void core::applyRuntimeConfig(const ServerRuntimeConfig& config)
{
    const QString trimmedHost = config.listenHost.trimmed();
    m_listenHost = trimmedHost.isEmpty() ? QStringLiteral("0.0.0.0") : trimmedHost;
    m_tcpPort = config.tcpPort == 0 ? TCP_PORT_IMPORTANT_DATA : config.tcpPort;
    m_kcpPort = config.kcpPort == 0 ? KCP_PORT_REALTIME_MOVE : config.kcpPort;
    m_sharedDataDirectory = config.dataDir.trimmed();
    g_configuredSharedDataDirectory = m_sharedDataDirectory;
}

/**
 * @brief 析构core对象并释放相关资源
 * @author Jaeger
 * @date 2025.3.28
 */
core:: ~core()
{
    //JaegerDebug();
    delete m_pTCPNet;
    delete m_pKcpNet;
}

/**
 * @brief 服务器启动入口，连通TCP，KCP，DB，FILE
 * @author Jaeger
 * @date 2025.3.28
 */
bool core::open() {
    QTextStream out(stdout); // 也可以用 qDebug().noquote()

    bool tcpOk = false;
    bool kcpOk = false;
    bool dbOk = false;
    bool fileOk = false;

    out << "\n===== Initializing Server Modules =====\n";

    // ---------------- TCP ----------------
    out << "-> TCP Network: ";
    tcpOk = m_pTCPNet->initNetWork(m_listenHost, m_tcpPort);
    out << (tcpOk ? GREEN "OK" RESET : RED "FAILED" RESET)
        << " (Host: " << m_listenHost << ", Port: " << m_tcpPort << ")\n";

    // ---------------- KCP ----------------
    out << "-> KCP Network: ";
    kcpOk = m_pKcpNet->initNetWork(m_listenHost, m_kcpPort);
    out << (kcpOk ? GREEN "OK" RESET : RED "FAILED" RESET)
        << " (Host: " << m_listenHost << ", Port: " << m_kcpPort << ")\n";

    // ---------------- DB ----------------
    out << "-> Database: ";
    dbOk = CheckDB();
    if (dbOk) {dbOk = ensureCoreAccountTables();}
    if (dbOk) {dbOk = ensureUserCredentialSchema();}
    if (dbOk) {dbOk = ensurePlayerStatColumns();}
    if (dbOk) {dbOk = ensureEquipmentColumns();}
    if (dbOk) {dbOk = ensureEquipmentStateTable();}
    if (dbOk) {dbOk = ensureProgressStateTable();}
    if (dbOk) {dbOk = ensureWarehouseStateTable();}
    if (dbOk) {dbOk = ensureInventoryJournalTable();}

    out << (dbOk ? GREEN "OK" RESET : RED "FAILED" RESET) << "\n";

    // ---------------- File ----------------
    out << "-> Chat History File: ";
    outFile.open("chat_history.txt", std::ios::app);
    fileOk = outFile.is_open();
    out << (fileOk ? GREEN "OK" RESET : RED "FAILED" RESET) << "\n";

    // ---------------- Summary ----------------
    m_moduleStatus.tcpOk = tcpOk;
    m_moduleStatus.kcpOk = kcpOk;
    m_moduleStatus.dbOk = dbOk;
    m_moduleStatus.fileOk = fileOk;
    out << "---------------------------------------\n";

    if (tcpOk && kcpOk && dbOk && fileOk) {
        if (m_dungeonTickTimer && !m_dungeonTickTimer->isActive()) {
            m_dungeonTickTimer->start();
        }
        loadPendingInventoryPersistenceJobs();
        out << GREEN "All modules initialized successfully!" RESET << "\n";
        return true;
    } else {
        out << RED "One or more modules failed to initialize!" RESET << "\n";
        return false;
    }
}

/**
 * @brief 程序启动时确保核心账号与基础角色表存在。
 * @author Jaeger
 * @date 2026.4.27
 */
bool core::ensureCoreAccountTables()
{
    CMySql sql;

    const char* accountTableSql =
        "CREATE TABLE IF NOT EXISTS user_account ("
        "u_id SERIAL PRIMARY KEY, "
        "u_name VARCHAR(64) NOT NULL UNIQUE, "
        "u_password VARCHAR(96) NOT NULL, "
        "u_tel BIGINT NOT NULL UNIQUE"
        ");";
    if (!sql.UpdateMySql(accountTableSql)) {
        return false;
    }

    const char* basicInfoTableSql =
        "CREATE TABLE IF NOT EXISTS user_basic_information ("
        "u_id INT PRIMARY KEY REFERENCES user_account(u_id) ON DELETE CASCADE, "
        "u_name VARCHAR(64) NOT NULL, "
        "u_health INT NOT NULL DEFAULT 100, "
        "u_mana INT NOT NULL DEFAULT 0, "
        "u_attackpower INT NOT NULL DEFAULT 0, "
        "u_magicattack INT NOT NULL DEFAULT 0, "
        "u_independentattack INT NOT NULL DEFAULT 0, "
        "u_attackrange INT NOT NULL DEFAULT 0, "
        "u_experience BIGINT NOT NULL DEFAULT 0, "
        "u_level INT NOT NULL DEFAULT 1, "
        "u_defence INT NOT NULL DEFAULT 0, "
        "u_magicdefence INT NOT NULL DEFAULT 0, "
        "u_strength INT NOT NULL DEFAULT 0, "
        "u_intelligence INT NOT NULL DEFAULT 0, "
        "u_vitality INT NOT NULL DEFAULT 0, "
        "u_spirit INT NOT NULL DEFAULT 0, "
        "u_critrate FLOAT NOT NULL DEFAULT 0, "
        "u_magiccritrate FLOAT NOT NULL DEFAULT 0, "
        "u_critdamage FLOAT NOT NULL DEFAULT 0, "
        "u_attackspeed FLOAT NOT NULL DEFAULT 0, "
        "u_movespeed FLOAT NOT NULL DEFAULT 0, "
        "u_castspeed FLOAT NOT NULL DEFAULT 0, "
        "u_position_x FLOAT NOT NULL DEFAULT 100, "
        "u_position_y FLOAT NOT NULL DEFAULT 100, "
        "u_equipment_weapon INT NOT NULL DEFAULT 0, "
        "u_equipment_head INT NOT NULL DEFAULT 0, "
        "u_equipment_body INT NOT NULL DEFAULT 0, "
        "u_equipment_legs INT NOT NULL DEFAULT 0, "
        "u_equipment_hands INT NOT NULL DEFAULT 0, "
        "u_equipment_shoes INT NOT NULL DEFAULT 0, "
        "u_equipment_shield INT NOT NULL DEFAULT 0"
        ");";
    if (!sql.UpdateMySql(basicInfoTableSql)) {
        return false;
    }

    const char* userItemTableSql =
        "CREATE TABLE IF NOT EXISTS user_item ("
        "u_id INT NOT NULL REFERENCES user_account(u_id) ON DELETE CASCADE, "
        "item_id INT NOT NULL, "
        "item_count INT NOT NULL DEFAULT 0, "
        "PRIMARY KEY (u_id, item_id)"
        ");";
    return sql.UpdateMySql(userItemTableSql);
}

/**
 * @brief 程序启动/初始化时自动检查数据库 user 表的密码字段是否满足新密码存储要求，不满足就自动升级表结构。
 * @author Jaeger
 * @date 2025.3.28
 */
bool core::ensureUserCredentialSchema()
{
    CMySql sql;
    std::list<std::string> result;
    bool passwordColumnExists = false;
    if (!querySchemaColumnExists(sql,
                                 QStringLiteral("user_account"),
                                 QStringLiteral("u_password"),
                                 &passwordColumnExists))
    {
        return false;
    }
    if (!passwordColumnExists) {
        qWarning() << "user_account.u_password column is missing; cannot validate credential schema.";
        return false;
    }

    result.clear();
    const QString lengthQuery =
        QStringLiteral("SELECT COALESCE(character_maximum_length, 0) "
                       "FROM information_schema.columns "
                       "WHERE table_schema = current_schema() "
                       "AND table_name = 'user_account' "
                       "AND column_name = 'u_password';");
    if (!selectQuery(sql, lengthQuery, 1, result)) {
        return false;
    }

    bool ok = false;
    const int currentLength =
        !result.empty() ? QString::fromStdString(result.front()).toInt(&ok) : 0;
    if (!ok) {
        qWarning() << "Failed to parse user_account.u_password column length from information_schema.";
        return false;
    }

    constexpr int kRequiredPasswordLength = 96;
    if (currentLength >= kRequiredPasswordLength) {
        return true;
    }

    const QString alterSql =
        QStringLiteral("ALTER TABLE user_account ALTER COLUMN u_password TYPE VARCHAR(%1);")
            .arg(kRequiredPasswordLength);
    if (!updateQuery(sql, alterSql)) {
        qWarning() << "Failed to expand user_account.u_password column to store hashed passwords.";
        return false;
    }

    qDebug() << "Expanded user_account.u_password column from"
             << currentLength
             << "to"
             << kRequiredPasswordLength;
    return true;
}

/**
 * @brief 查询moduleStatus相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
ServerModuleStatus core::moduleStatus() const
{
    return m_moduleStatus;
}

/**
 * @brief 查询monitorSnapshot相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
ServerMonitorSnapshot core::monitorSnapshot() const
{
    ServerMonitorSnapshot snapshot;
    snapshot.capturedAt = QDateTime::currentDateTime();
    snapshot.modules = m_moduleStatus;
    snapshot.tcp = m_pTCPNet ? m_pTCPNet->monitorStats() : TcpMonitorStats{};
    snapshot.kcp = m_pKcpNet ? m_pKcpNet->monitorStats() : KcpMonitorStats{};
    snapshot.trackedPlayerCount = players.size();
    snapshot.partyCount = m_partyStates.size();
    snapshot.pendingInviteCount = m_pendingPartyInvites.size();
    snapshot.dungeonRoomCount = m_dungeonRooms.size();

    for (const auto& trackedPlayer : players) {
        const bool online = isClientOnline(trackedPlayer.clientId);
        if (online) {
            ++snapshot.onlineTrackedPlayerCount;
        }

        PlayerMonitorEntry entry;
        entry.clientId = trackedPlayer.clientId;
        entry.playerId = trackedPlayer.player_UserId;
        entry.name = trackedPlayer.playerName;
        entry.mapId = trackedPlayer.mapId;
        entry.instanceId = trackedPlayer.instanceId;
        entry.level = trackedPlayer.level;
        entry.health = trackedPlayer.health;
        entry.mana = trackedPlayer.mana;
        entry.positionX = trackedPlayer.positionX;
        entry.positionY = trackedPlayer.positionY;
        entry.online = online;
        if (const PartyRuntimeState* party = findPartyByPlayerId(trackedPlayer.player_UserId)) {
            entry.partyId = party->partyId;
        }
        snapshot.players.push_back(entry);
    }

    std::sort(snapshot.players.begin(),
              snapshot.players.end(),
              [](const PlayerMonitorEntry& lhs, const PlayerMonitorEntry& rhs) {
                  if (lhs.online != rhs.online) {
                      return lhs.online > rhs.online;
                  }
                  if (lhs.mapId != rhs.mapId) {
                      return lhs.mapId < rhs.mapId;
                  }
                  return lhs.playerId < rhs.playerId;
              });

    for (auto it = m_partyStates.constBegin(); it != m_partyStates.constEnd(); ++it) {
        const PartyRuntimeState& party = it.value();
        PartyMonitorEntry entry;
        entry.partyId = party.partyId;
        entry.leaderPlayerId = party.leaderPlayerId;
        entry.sharedInstanceId = party.sharedInstanceId;
        entry.memberCount = party.memberPlayerIds.size();

        QStringList memberNames;
        for (int memberPlayerId : party.memberPlayerIds) {
            const Player_Information* member = findTrackedPlayer(memberPlayerId);
            if (!member) {
                memberNames << QStringLiteral("#%1").arg(memberPlayerId);
                continue;
            }
            memberNames << QStringLiteral("%1(%2)")
                               .arg(member->playerName.isEmpty() ? QStringLiteral("Unknown") : member->playerName)
                               .arg(memberPlayerId);
            if (memberPlayerId == party.leaderPlayerId) {
                entry.leaderName = member->playerName;
            }
        }
        if (entry.leaderName.isEmpty()) {
            entry.leaderName = QStringLiteral("Player %1").arg(party.leaderPlayerId);
        }
        entry.memberNames = memberNames.join(QStringLiteral(", "));
        snapshot.parties.push_back(entry);
    }

    std::sort(snapshot.parties.begin(),
              snapshot.parties.end(),
              [](const PartyMonitorEntry& lhs, const PartyMonitorEntry& rhs) {
                  return lhs.partyId < rhs.partyId;
              });

    for (auto it = m_dungeonRooms.constBegin(); it != m_dungeonRooms.constEnd(); ++it) {
        const DungeonRoomState& room = it.value();
        DungeonMonitorEntry entry;
        entry.mapId = room.mapId;
        entry.instanceId = room.instanceId;
        entry.scaledLevel = room.scaledLevel;
        entry.totalMonsterCount = room.totalMonsterCount;
        entry.aliveMonsterCount = room.aliveMonsterCount;
        entry.participantCount = room.participantUpdatedAtMs.size();
        entry.onlinePlayerCount = collectScopedClientIds(room.mapId, room.instanceId).size();
        entry.completed = room.completed;
        if (entry.onlinePlayerCount > 0) {
            ++snapshot.activeDungeonRoomCount;
        }
        snapshot.dungeons.push_back(entry);
    }

    std::sort(snapshot.dungeons.begin(),
              snapshot.dungeons.end(),
              [](const DungeonMonitorEntry& lhs, const DungeonMonitorEntry& rhs) {
                  if (lhs.mapId != rhs.mapId) {
                      return lhs.mapId < rhs.mapId;
                  }
                  return lhs.instanceId < rhs.instanceId;
              });

    return snapshot;
}

/**
 * @brief 处理findTrackedPlayer相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
Player_Information* core::findTrackedPlayer(int userId)
{
    for (auto& trackedPlayer : players) {
        if (trackedPlayer.player_UserId == userId) {
            return &trackedPlayer;
        }
    }
    return nullptr;
}

/**
 * @brief 处理findTrackedPlayer相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
const Player_Information* core::findTrackedPlayer(int userId) const
{
    for (const auto& trackedPlayer : players) {
        if (trackedPlayer.player_UserId == userId) {
            return &trackedPlayer;
        }
    }
    return nullptr;
}

/**
 * @brief 处理findTrackedPlayerByName相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
Player_Information* core::findTrackedPlayerByName(const QString& playerName)
{
    const QString normalizedName = playerName.trimmed();
    if (normalizedName.isEmpty()) {
        return nullptr;
    }

    for (auto& trackedPlayer : players) {
        if (!isClientOnline(trackedPlayer.clientId)) {
            continue;
        }
        if (trackedPlayer.playerName.compare(normalizedName, Qt::CaseInsensitive) == 0) {
            return &trackedPlayer;
        }
    }
    return nullptr;
}

/**
 * @brief 处理findTrackedPlayerByName相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
const Player_Information* core::findTrackedPlayerByName(const QString& playerName) const
{
    const QString normalizedName = playerName.trimmed();
    if (normalizedName.isEmpty()) {
        return nullptr;
    }

    for (const auto& trackedPlayer : players) {
        if (!isClientOnline(trackedPlayer.clientId)) {
            continue;
        }
        if (trackedPlayer.playerName.compare(normalizedName, Qt::CaseInsensitive) == 0) {
            return &trackedPlayer;
        }
    }
    return nullptr;
}

/**
 * @brief 处理findTrackedPlayerByClientId相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
Player_Information* core::findTrackedPlayerByClientId(quint64 clientId)
{
    for (auto& trackedPlayer : players) {
        if (trackedPlayer.clientId == clientId) {
            return &trackedPlayer;
        }
    }
    return nullptr;
}

/**
 * @brief 处理findTrackedPlayerByClientId相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
const Player_Information* core::findTrackedPlayerByClientId(quint64 clientId) const
{
    for (const auto& trackedPlayer : players) {
        if (trackedPlayer.clientId == clientId) {
            return &trackedPlayer;
        }
    }
    return nullptr;
}

/**
 * @brief 查询boundPlayerIdForClient相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
int core::boundPlayerIdForClient(quint64 clientId) const
{
    const auto it = m_clientPlayerBindings.constFind(clientId);
    if (it == m_clientPlayerBindings.constEnd()) {
        return 0;
    }
    return qMax(0, it.value());
}

/**
 * @brief 判断isClientBoundToPlayer相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool core::isClientBoundToPlayer(quint64 clientId, int playerId) const
{
    return playerId > 0 && boundPlayerIdForClient(clientId) == playerId;
}

/**
 * @brief 处理bindClientToPlayer相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::bindClientToPlayer(quint64 clientId, int playerId)
{
    if (clientId == 0 || playerId <= 0) {
        return;
    }
    m_clientPlayerBindings.insert(clientId, playerId);
}

/**
 * @brief 处理clearClientBinding相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::clearClientBinding(quint64 clientId)
{
    m_clientPlayerBindings.remove(clientId);
}

/**
 * @brief 查询findBoundTrackedPlayer相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
Player_Information* core::findBoundTrackedPlayer(quint64 clientId, int expectedPlayerId)
{
    const int boundPlayerId = boundPlayerIdForClient(clientId);
    if (boundPlayerId <= 0) {
        return nullptr;
    }
    if (expectedPlayerId > 0 && expectedPlayerId != boundPlayerId) {
        return nullptr;
    }
    return findTrackedPlayer(boundPlayerId);
}

/**
 * @brief 查询findBoundTrackedPlayer相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
const Player_Information* core::findBoundTrackedPlayer(quint64 clientId, int expectedPlayerId) const
{
    const int boundPlayerId = boundPlayerIdForClient(clientId);
    if (boundPlayerId <= 0) {
        return nullptr;
    }
    if (expectedPlayerId > 0 && expectedPlayerId != boundPlayerId) {
        return nullptr;
    }
    return findTrackedPlayer(boundPlayerId);
}

/**
 * @brief 处理findPartyById相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
PartyRuntimeState* core::findPartyById(int partyId)
{
    auto it = m_partyStates.find(partyId);
    return it == m_partyStates.end() ? nullptr : &it.value();
}

/**
 * @brief 处理findPartyById相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
const PartyRuntimeState* core::findPartyById(int partyId) const
{
    auto it = m_partyStates.constFind(partyId);
    return it == m_partyStates.constEnd() ? nullptr : &it.value();
}

/**
 * @brief 处理findPartyByPlayerId相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
PartyRuntimeState* core::findPartyByPlayerId(int playerId)
{
    return findPartyById(m_playerPartyIds.value(playerId, 0));
}

/**
 * @brief 处理findPartyByPlayerId相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
const PartyRuntimeState* core::findPartyByPlayerId(int playerId) const
{
    return findPartyById(m_playerPartyIds.value(playerId, 0));
}

/**
 * @brief 判断isPartySharedMap相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool core::isPartySharedMap(const QString& mapId) const
{
    const QString normalizedMapId = mapId.trimmed();
    return normalizedMapId.compare(QStringLiteral("MonsterField"), Qt::CaseInsensitive) == 0
           || normalizedMapId.compare(QStringLiteral("SanctuaryPass"), Qt::CaseInsensitive) == 0
           || normalizedMapId.compare(QStringLiteral("NorthTestDungeon"), Qt::CaseInsensitive) == 0
           || normalizedMapId.compare(QStringLiteral("WestTestDungeon"), Qt::CaseInsensitive) == 0
           || normalizedMapId.compare(QStringLiteral("EastTestDungeon"), Qt::CaseInsensitive) == 0;
}

/**
 * @brief 判断isPartyDungeonMap相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool core::isPartyDungeonMap(const QString& mapId) const
{
    const QString normalizedMapId = mapId.trimmed();
    return normalizedMapId.compare(QStringLiteral("SanctuaryPass"), Qt::CaseInsensitive) == 0
           || normalizedMapId.compare(QStringLiteral("NorthTestDungeon"), Qt::CaseInsensitive) == 0
           || normalizedMapId.compare(QStringLiteral("WestTestDungeon"), Qt::CaseInsensitive) == 0
           || normalizedMapId.compare(QStringLiteral("EastTestDungeon"), Qt::CaseInsensitive) == 0;
}

/**
 * @brief 处理resolveAuthoritativeInstanceIdForPlayerMap相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
qint64 core::resolveAuthoritativeInstanceIdForPlayerMap(int playerId,
                                                        const QString& mapId,
                                                        qint64 fallbackInstanceId) const
{
    const QString normalizedMapId = mapId.trimmed().isEmpty()
                                        ? QStringLiteral("BornWorld")
                                        : mapId.trimmed();
    if (!isPartySharedMap(normalizedMapId)) {
        return qMax<qint64>(0, fallbackInstanceId);
    }

    if (const PartyRuntimeState* party = findPartyByPlayerId(playerId)) {
        return qMax<qint64>(1, party->sharedInstanceId);
    }

    if (playerId > 0) {
        return playerId;
    }

    return qMax<qint64>(0, fallbackInstanceId);
}

/**
 * @brief 处理sendPartyStateToPlayer相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::sendPartyStateToPlayer(int playerId,
                                  quint16 action,
                                  int result,
                                  const QString& message,
                                  bool includePendingInvite)
{
    Player_Information* trackedPlayer = findTrackedPlayer(playerId);
    if (!trackedPlayer || !isClientOnline(trackedPlayer->clientId)) {
        return;
    }

    STRU_PARTY_STATE_RS rs{};
    rs.result = result;
    rs.action = action;
    rs.partyId = 0;
    rs.leaderPlayerId = 0;
    rs.sharedInstanceId = 0;
    std::strncpy(rs.message, message.toUtf8().constData(), sizeof(rs.message) - 1);

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (includePendingInvite) {
        auto inviteIt = m_pendingPartyInvites.find(playerId);
        if (inviteIt != m_pendingPartyInvites.end()) {
            const PendingPartyInviteState invite = inviteIt.value();
            if (invite.expiresAtMs > 0 && invite.expiresAtMs <= nowMs) {
                inviteIt = m_pendingPartyInvites.erase(inviteIt);
            } else {
                rs.stateFlags |= _party_state_flag_has_pending_invite;
                rs.pendingInviteFromPlayerId = invite.inviterPlayerId;
                std::strncpy(rs.pendingInviteFromName,
                             invite.inviterName.toUtf8().constData(),
                             sizeof(rs.pendingInviteFromName) - 1);
            }
        }
    }

    const PartyRuntimeState* party = findPartyByPlayerId(playerId);
    if (party) {
        const QVector<int> memberIds = party->memberPlayerIds;
        rs.partyId = party->partyId;
        rs.leaderPlayerId = party->leaderPlayerId;
        rs.sharedInstanceId = party->sharedInstanceId;

        int memberIndex = 0;
        for (int memberPlayerId : memberIds) {
            if (memberIndex >= MAX_PARTY_MEMBER_NUM) {
                break;
            }

            const Player_Information* member = findTrackedPlayer(memberPlayerId);
            PartyMemberSyncEntry& entry = rs.members[memberIndex];
            entry.playerId = memberPlayerId;
            QString memberName = member ? member->playerName : QStringLiteral("未知成员");
            QString memberMapId = member ? member->mapId : QStringLiteral("BornWorld");
            qint64 memberInstanceId = member ? member->instanceId : 0;
            std::strncpy(entry.playerName, memberName.toUtf8().constData(), sizeof(entry.playerName) - 1);
            std::strncpy(entry.mapId, memberMapId.toUtf8().constData(), sizeof(entry.mapId) - 1);
            entry.instanceId = memberInstanceId;
            if (memberPlayerId == party->leaderPlayerId) {
                entry.memberFlags |= _party_member_flag_leader;
            }
            if (memberPlayerId == playerId) {
                entry.memberFlags |= _party_member_flag_self;
            }
            if (member && isClientOnline(member->clientId)) {
                entry.memberFlags |= _party_member_flag_online;
            }
            ++memberIndex;
        }
        rs.memberCount = memberIndex;
    }

    const QByteArray packet = PacketBuilder::build(_default_protocol_party_state_rs, rs);
    emit sendToClient(trackedPlayer->clientId, packet);
}

/**
 * @brief 处理broadcastPartyState相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::broadcastPartyState(int partyId,
                               quint16 action,
                               int result,
                               const QString& message,
                               int excludedPlayerId)
{
    const PartyRuntimeState* party = findPartyById(partyId);
    if (!party) {
        return;
    }

    const QVector<int> memberIds = party->memberPlayerIds;
    for (int memberPlayerId : memberIds) {
        if (memberPlayerId == excludedPlayerId) {
            continue;
        }
        sendPartyStateToPlayer(memberPlayerId, action, result, message, true);
    }
}

/**
 * @brief 处理clearPendingInvitesForParty相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::clearPendingInvitesForParty(int partyId)
{
    for (auto it = m_pendingPartyInvites.begin(); it != m_pendingPartyInvites.end();) {
        if (it.value().partyId == partyId) {
            it = m_pendingPartyInvites.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * @brief 处理sendPartyDungeonEntryToPlayer相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::sendPartyDungeonEntryToPlayer(int playerId,
                                         const PartyRuntimeState& party,
                                         const QString& mapId,
                                         qint64 instanceId,
                                         float x,
                                         float y,
                                         const QString& message)
{
    Player_Information* trackedPlayer = findTrackedPlayer(playerId);
    if (!trackedPlayer || !isClientOnline(trackedPlayer->clientId)) {
        return;
    }

    STRU_PARTY_DUNGEON_RS rs{};
    rs.action = _party_dungeon_action_enter;
    rs.result = 0;
    rs.partyId = party.partyId;
    rs.leaderPlayerId = party.leaderPlayerId;
    rs.instanceId = qMax<qint64>(0, instanceId);
    rs.x = x;
    rs.y = y;
    std::strncpy(rs.mapId, mapId.toUtf8().constData(), sizeof(rs.mapId) - 1);
    std::strncpy(rs.message, message.toUtf8().constData(), sizeof(rs.message) - 1);

    const QByteArray packet = PacketBuilder::build(_default_protocol_party_dungeon_rs, rs);
    emit sendToClient(trackedPlayer->clientId, packet);
}

/**
 * @brief 处理broadcastPartyDungeonEntry相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::broadcastPartyDungeonEntry(int initiatorPlayerId,
                                      const QString& mapId,
                                      qint64 instanceId,
                                      float x,
                                      float y)
{
    const PartyRuntimeState* party = findPartyByPlayerId(initiatorPlayerId);
    const Player_Information* initiator = findTrackedPlayer(initiatorPlayerId);
    if (!party || !initiator) {
        return;
    }

    const QString message = QStringLiteral("%1 已开启队伍副本 %2，正在同步全队进入。")
                                .arg(initiator->playerName, mapId);
    const QVector<int> memberIds = party->memberPlayerIds;
    for (int memberPlayerId : memberIds) {
        if (memberPlayerId == initiatorPlayerId) {
            continue;
        }

        const Player_Information* member = findTrackedPlayer(memberPlayerId);
        if (member
            && member->mapId.compare(mapId, Qt::CaseInsensitive) == 0
            && member->instanceId == instanceId)
        {
            continue;
        }

        sendPartyDungeonEntryToPlayer(memberPlayerId, *party, mapId, instanceId, x, y, message);
    }
}

/**
 * @brief 处理upsertTrackedPlayer相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::upsertTrackedPlayer(quint64 clientId,
                               int userId,
                               const QString& playerName,
                               const QString& mapId,
                               qint64 instanceId,
                               float x,
                               float y,
                               int dir)
{
    QString normalizedMapId = mapId.trimmed();
    if (normalizedMapId.isEmpty()) {
        normalizedMapId = QStringLiteral("BornWorld");
    }
    const qint64 normalizedInstanceId = qMax<qint64>(0, instanceId);

    if (Player_Information* trackedPlayer = findTrackedPlayer(userId)) {
        trackedPlayer->clientId = clientId;
        if (!playerName.trimmed().isEmpty()) {
            trackedPlayer->playerName = playerName;
        }
        trackedPlayer->mapId = normalizedMapId;
        trackedPlayer->instanceId = normalizedInstanceId;
        trackedPlayer->positionX = x;
        trackedPlayer->positionY = y;
        trackedPlayer->direction = dir;
        return;
    }

    const CombatBalance::PlayerStats baseline = CombatBalance::playerStats(1);
    players.emplace_back(userId,
                         playerName,
                         1,
                         0,
                         0,
                         baseline.maxHealth,
                         baseline.maxMana,
                         baseline.attack,
                         baseline.magicAttack,
                         baseline.independentAttack,
                         baseline.defence,
                         baseline.magicDefence,
                         baseline.strength,
                         baseline.intelligence,
                         baseline.vitality,
                         baseline.spirit,
                         baseline.critRate,
                         baseline.magicCritRate,
                         baseline.critDamage,
                         baseline.attackSpeed,
                         baseline.moveSpeed,
                         baseline.castSpeed,
                         baseline.attackRange,
                         x,
                         y,
                         clientId,
                             normalizedMapId,
                             normalizedInstanceId,
                             dir);
    if (!players.empty()) {
        primeTrackedPlayerInventoryVersion(players.back());
    }
}

/**
 * @brief 处理handleClientDisconnected相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::handleClientDisconnected(quint64 clientId)
{
    const Player_Information* disconnectedPlayer = findTrackedPlayerByClientId(clientId);
    clearClientBinding(clientId);
    if (!disconnectedPlayer) {
        return;
    }

    const int playerId = disconnectedPlayer->player_UserId;
    const QString playerName = disconnectedPlayer->playerName;
    const QString previousMapId = disconnectedPlayer->mapId.trimmed().isEmpty()
                                      ? QStringLiteral("BornWorld")
                                      : disconnectedPlayer->mapId.trimmed();
    const qint64 previousInstanceId = qMax<qint64>(0, disconnectedPlayer->instanceId);

    int inviterPlayerIdToRefresh = 0;
    if (const auto inviteIt = m_pendingPartyInvites.constFind(playerId);
        inviteIt != m_pendingPartyInvites.constEnd())
    {
        inviterPlayerIdToRefresh = inviteIt.value().inviterPlayerId;
    }

    QVector<int> inviteTargetsToRefresh;
    for (auto it = m_pendingPartyInvites.begin(); it != m_pendingPartyInvites.end();) {
        if (it.key() == playerId || it.value().inviterPlayerId == playerId) {
            if (it.key() != playerId && !inviteTargetsToRefresh.contains(it.key())) {
                inviteTargetsToRefresh.push_back(it.key());
            }
            it = m_pendingPartyInvites.erase(it);
        } else {
            ++it;
        }
    }

    const int partyId = m_playerPartyIds.value(playerId, 0);
    PartyRuntimeState* party = findPartyById(partyId);
    if (party) {
        party->memberPlayerIds.removeAll(playerId);
        m_playerPartyIds.remove(playerId);

        if (party->memberPlayerIds.isEmpty()) {
            clearPendingInvitesForParty(partyId);
            m_partyStates.remove(partyId);
        } else {
            QString message = QStringLiteral("%1 已离线，已从队伍中移除。").arg(playerName);
            if (party->leaderPlayerId == playerId) {
                party->leaderPlayerId = party->memberPlayerIds.front();
                const Player_Information* newLeader = findTrackedPlayer(party->leaderPlayerId);
                const QString newLeaderName = newLeader ? newLeader->playerName : QStringLiteral("新队长");
                message += QStringLiteral(" %1 成为了新的队长。").arg(newLeaderName);
            }
            broadcastPartyState(partyId, _party_action_leave, 0, message);
        }
    } else {
        m_playerPartyIds.remove(playerId);
    }

    const QString skillKeyPrefix = QStringLiteral("%1#").arg(playerId);
    for (auto it = m_skillHitWindows.begin(); it != m_skillHitWindows.end();) {
        if (it.key().startsWith(skillKeyPrefix)) {
            it = m_skillHitWindows.erase(it);
        } else {
            ++it;
        }
    }

    auto dazuoTimerIt = m_mapDazuoTimer.find(playerId);
    if (dazuoTimerIt != m_mapDazuoTimer.end() && dazuoTimerIt->second) {
        dazuoTimerIt->second->stop();
        delete dazuoTimerIt->second;
        m_mapDazuoTimer.erase(dazuoTimerIt);
    }
    m_mapPlayerInfo.erase(playerId);

    players.erase(std::remove_if(players.begin(),
                                 players.end(),
                                 [playerId, clientId](const Player_Information& trackedPlayer) {
                                     return trackedPlayer.player_UserId == playerId
                                            || trackedPlayer.clientId == clientId;
                                 }),
                  players.end());

    const QString inviteCancelledMessage = QStringLiteral("%1 已离线，队伍邀请已自动取消。").arg(playerName);
    if (inviterPlayerIdToRefresh > 0 && inviterPlayerIdToRefresh != playerId) {
        sendPartyStateToPlayer(inviterPlayerIdToRefresh,
                               _party_action_decline,
                               0,
                               inviteCancelledMessage,
                               true);
    }
    for (int inviteTargetPlayerId : inviteTargetsToRefresh) {
        if (inviteTargetPlayerId == playerId) {
            continue;
        }
        sendPartyStateToPlayer(inviteTargetPlayerId,
                               _party_action_decline,
                               0,
                               inviteCancelledMessage,
                               true);
    }

    broadcastScopedLocationSnapshot(previousMapId, previousInstanceId);
    resetDungeonRoomIfNoActivePlayers(previousMapId, previousInstanceId);

    qInfo() << "Cleaned tracked runtime state for disconnected client"
            << clientId << "playerId" << playerId
            << "scope" << previousMapId << previousInstanceId;
}

/**
 * @brief 广播broadcastScopedLocationSnapshot相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::broadcastScopedLocationSnapshot(const QString& mapId, qint64 instanceId)
{
    const QString normalizedMapId = mapId.trimmed().isEmpty() ? QStringLiteral("BornWorld") : mapId.trimmed();
    const qint64 normalizedInstanceId = qMax<qint64>(0, instanceId);

    STRU_LOCATION_RS locationRs{};
    int trackedCount = 0;
    for (const auto& trackedPlayer : players) {
        if (trackedCount >= 50) {
            break;
        }
        if (!isClientOnline(trackedPlayer.clientId)) {
            continue;
        }
        if (trackedPlayer.mapId.compare(normalizedMapId, Qt::CaseInsensitive) != 0
            || trackedPlayer.instanceId != normalizedInstanceId)
        {
            continue;
        }

        locationRs.players[trackedCount].player_UserId = trackedPlayer.player_UserId;
        std::strncpy(locationRs.players[trackedCount].player_Name,
                     trackedPlayer.playerName.toUtf8().constData(),
                     sizeof(locationRs.players[trackedCount].player_Name) - 1);
        locationRs.players[trackedCount].x = trackedPlayer.positionX;
        locationRs.players[trackedCount].y = trackedPlayer.positionY;
        locationRs.players[trackedCount].dir = trackedPlayer.direction;
        std::strncpy(locationRs.players[trackedCount].mapId,
                     trackedPlayer.mapId.toUtf8().constData(),
                     sizeof(locationRs.players[trackedCount].mapId) - 1);
        locationRs.players[trackedCount].instanceId = trackedPlayer.instanceId;
        ++trackedCount;
    }
    locationRs.playerCount = trackedCount;

    const QByteArray packet = PacketBuilder::build(_default_protocol_location_rs, locationRs);
    const QVector<quint64> scopedClients = collectScopedClientIds(normalizedMapId, normalizedInstanceId);
    for (quint64 scopedClientId : scopedClients) {
        if (m_pKcpNet && m_pKcpNet->isKcpConnected(scopedClientId)) {
            emit sendToClientKcp(scopedClientId, packet);
        } else {
            emit sendToClient(scopedClientId, packet);
        }
    }
}

/**
 * @brief 清理pruneExpiredSkillHitWindows相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::pruneExpiredSkillHitWindows(qint64 nowMs)
{
    for (auto it = m_skillHitWindows.begin(); it != m_skillHitWindows.end();) {
        if (it.value().expiresAtMs > 0 && it.value().expiresAtMs <= nowMs) {
            it = m_skillHitWindows.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * @brief 处理isClientOnline相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool core::isClientOnline(quint64 clientId) const
{
    return m_pTCPNet && m_pTCPNet->getAllClientIds().contains(clientId);
}

/**
 * @brief 处理collectScopedClientIds相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
QVector<quint64> core::collectScopedClientIds(const QString& mapId, qint64 instanceId) const
{
    QVector<quint64> scopedClientIds;
    const QString normalizedMapId = mapId.trimmed().isEmpty() ? QStringLiteral("BornWorld") : mapId.trimmed();
    const qint64 normalizedInstanceId = qMax<qint64>(0, instanceId);

    for (const auto& trackedPlayer : players) {
        if (!isClientOnline(trackedPlayer.clientId)) {
            continue;
        }
        if (trackedPlayer.mapId.compare(normalizedMapId, Qt::CaseInsensitive) != 0
            || trackedPlayer.instanceId != normalizedInstanceId)
        {
            continue;
        }
        if (!scopedClientIds.contains(trackedPlayer.clientId)) {
            scopedClientIds.push_back(trackedPlayer.clientId);
        }
    }
    return scopedClientIds;
}

/**
 * @brief 处理collectScopedPlayers相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
QVector<Player_Information*> core::collectScopedPlayers(const QString& mapId, qint64 instanceId)
{
    QVector<Player_Information*> scopedPlayers;
    const QString normalizedMapId = mapId.trimmed().isEmpty() ? QStringLiteral("BornWorld") : mapId.trimmed();
    const qint64 normalizedInstanceId = qMax<qint64>(0, instanceId);

    for (auto& trackedPlayer : players) {
        if (!isClientOnline(trackedPlayer.clientId)) {
            continue;
        }
        if (trackedPlayer.mapId.compare(normalizedMapId, Qt::CaseInsensitive) != 0
            || trackedPlayer.instanceId != normalizedInstanceId)
        {
            continue;
        }
        scopedPlayers.push_back(&trackedPlayer);
    }
    return scopedPlayers;
}

/**
 * @brief 记录noteDungeonParticipant相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::noteDungeonParticipant(DungeonRoomState& room, int playerId, qint64 nowMs, int damage)
{
    if (playerId <= 0) {
        return;
    }
    ParticipationRecord& record = room.participantUpdatedAtMs[playerId];
    record.lastUpdatedAtMs = nowMs;
    record.totalDamage += qMax(0, damage);
    record.hitCount += 1;
}

/**
 * @brief 收集collectEligibleRewardRecipients相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
QVector<Player_Information*> core::collectEligibleRewardRecipients(
    const DungeonRoomState& room,
    int primaryPlayerId,
    const QHash<int, ParticipationRecord>& participantUpdatedAtMs,
    qint64 nowMs,
    qint64 participationWindowMs,
    int minimumDamage,
    int minimumHitCount)
{
    QVector<Player_Information*> recipients;
    QHash<int, bool> seenPlayerIds;

    auto tryAppendRecipient = [&](int playerId) {
        if (playerId <= 0 || seenPlayerIds.contains(playerId)) {
            return;
        }
        Player_Information* trackedPlayer = findTrackedPlayer(playerId);
        if (!trackedPlayer || !isClientOnline(trackedPlayer->clientId)) {
            return;
        }
        if (trackedPlayer->mapId.compare(room.mapId, Qt::CaseInsensitive) != 0
            || trackedPlayer->instanceId != room.instanceId)
        {
            return;
        }
        seenPlayerIds.insert(playerId, true);
        recipients.push_back(trackedPlayer);
    };

    if (const PartyRuntimeState* party = findPartyByPlayerId(primaryPlayerId)) {
        for (int memberPlayerId : party->memberPlayerIds) {
            tryAppendRecipient(memberPlayerId);
        }
    } else {
        for (auto it = participantUpdatedAtMs.constBegin(); it != participantUpdatedAtMs.constEnd(); ++it) {
            const ParticipationRecord& record = it.value();
            if (record.lastUpdatedAtMs <= 0
                || (participationWindowMs > 0 && (nowMs - record.lastUpdatedAtMs) > participationWindowMs))
            {
                continue;
            }
            if (record.totalDamage < minimumDamage && record.hitCount < minimumHitCount) {
                continue;
            }
            tryAppendRecipient(it.key());
        }
    }

    if (recipients.isEmpty()) {
        tryAppendRecipient(primaryPlayerId);
    }

    return recipients;
}

/**
 * @brief 生成roomKeyFor相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
QString core::roomKeyFor(const QString& mapId, qint64 instanceId) const
{
    const QString normalizedMapId = mapId.trimmed().isEmpty() ? QStringLiteral("BornWorld") : mapId.trimmed();
    return QStringLiteral("%1#%2").arg(normalizedMapId.toLower()).arg(qMax<qint64>(0, instanceId));
}

/**
 * @brief 解析resolveMonsterMapPath相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
QString core::resolveMonsterMapPath(const QString& mapId) const
{
    const QString filename = QStringLiteral("%1.tmx").arg(mapId.trimmed());
    return JaegerShared::resolveSharedDataFilePath(filename, {m_sharedDataDirectory});
}

/**
 * @brief 加载loadMonsterSpawnDefinitions相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
QVector<MonsterSpawnDefinition> core::loadMonsterSpawnDefinitions(const QString& mapId) const
{
    QVector<MonsterSpawnDefinition> definitions;
    const QString mapPath = resolveMonsterMapPath(mapId);
    if (mapPath.isEmpty()) {
        qWarning() << "Failed to resolve monster map path for map:" << mapId
                   << "currentDir=" << QDir::currentPath()
                   << "appDir=" << QCoreApplication::applicationDirPath();
        return definitions;
    }

    XMLDocument doc;
    if (doc.LoadFile(mapPath.toStdString().c_str()) != XML_SUCCESS) {
        qWarning() << "Failed to parse dungeon monster map:" << mapPath;
        return definitions;
    }

    XMLElement* mapElement = doc.FirstChildElement("map");
    if (!mapElement) {
        return definitions;
    }

    const int fallbackLevel = qMax(1, readMapIntProperty(mapElement, QStringLiteral("RecommendedLevel"), 1));
    for (XMLElement* groupElement = mapElement->FirstChildElement("objectgroup");
         groupElement;
         groupElement = groupElement->NextSiblingElement("objectgroup"))
    {
        const QString groupName = groupElement->Attribute("name") ? QString::fromUtf8(groupElement->Attribute("name")) : QString();
        if (groupName.compare(QStringLiteral("Npcs"), Qt::CaseInsensitive) != 0
            && groupName.compare(QStringLiteral("NPCs"), Qt::CaseInsensitive) != 0)
        {
            continue;
        }

        for (XMLElement* objectElement = groupElement->FirstChildElement("object");
             objectElement;
             objectElement = objectElement->NextSiblingElement("object"))
        {
            const QString npcClass = readObjectProperty(objectElement, QStringLiteral("class")).trimmed().toLower();
            const QString npcType = readObjectProperty(objectElement, QStringLiteral("npcType")).trimmed().toLower();
            const bool isAttackable = npcType == QStringLiteral("attackable")
                                      || npcType == QStringLiteral("1")
                                      || npcType == QStringLiteral("true")
                                      || (npcType.isEmpty()
                                          && (npcClass == QStringLiteral("dummy")
                                              || npcClass == QStringLiteral("mob")
                                              || npcClass == QStringLiteral("monster")));
            if (!isAttackable) {
                continue;
            }

            MonsterSpawnDefinition definition;
            definition.runtimeId = readIntProperty(objectElement,
                                                   QStringLiteral("runtimeId"),
                                                   objectElement->IntAttribute("id", 0));
            if (definition.runtimeId <= 0) {
                continue;
            }

            const QString displayName = readObjectProperty(objectElement, QStringLiteral("displayName")).trimmed();
            const QString npcId = readObjectProperty(objectElement, QStringLiteral("npcId")).trimmed();
            const QString objectName = objectElement->Attribute("name") ? QString::fromUtf8(objectElement->Attribute("name")) : QString();
            definition.displayName = normalizeMonsterName(displayName.isEmpty() ? (npcId.isEmpty() ? objectName : npcId) : displayName,
                                                          definition.runtimeId);
            definition.spawnPos = QPointF(objectElement->FloatAttribute("x"), objectElement->FloatAttribute("y"));
            definition.monsterLevel = qMax(1, readIntProperty(objectElement, QStringLiteral("monsterLevel"), fallbackLevel));
            definition.monsterTier = CombatBalance::parseMonsterTier(readObjectProperty(objectElement, QStringLiteral("monsterTier")));

            const bool autoBalance = readBoolProperty(objectElement,
                                                      QStringLiteral("autoBalance"),
                                                      npcClass != QStringLiteral("dummy"));
            definition.maxHealth = qMax(1, readIntProperty(objectElement, QStringLiteral("maxHealth"), 0));
            definition.attack = qMax(0, readIntProperty(objectElement, QStringLiteral("attack"), 0));
            if (autoBalance || definition.maxHealth <= 0 || definition.attack <= 0) {
                const CombatBalance::MonsterStats stats = CombatBalance::monsterStats(definition.monsterLevel,
                                                                                      definition.monsterTier);
                if (definition.maxHealth <= 0) {
                    definition.maxHealth = qMax(1, stats.maxHealth);
                }
                if (definition.attack <= 0) {
                    definition.attack = qMax(0, stats.attack);
                }
            }

            const bool enableChase = readBoolProperty(objectElement,
                                                      QStringLiteral("enableChase"),
                                                      npcClass != QStringLiteral("dummy"));
            if (enableChase) {
                definition.aggroRange = readFloatProperty(objectElement, QStringLiteral("aggroRange"), 220.0f);
                definition.leashRange = qMax(definition.aggroRange + 24.0f,
                                             readFloatProperty(objectElement, QStringLiteral("leashRange"), 380.0f));
                definition.moveSpeed = qMax(0.5f, readFloatProperty(objectElement, QStringLiteral("moveSpeed"), 2.6f));
            } else {
                definition.aggroRange = 0.0f;
                definition.leashRange = 0.0f;
                definition.moveSpeed = 0.0f;
            }
            definition.attackRange = 28.0f;
            definition.dropGoldRate = qMax(0.10f, readFloatProperty(objectElement, QStringLiteral("dropGoldRate"), 1.0f));
            definition.dropMaterialRate = qMax(0.10f, readFloatProperty(objectElement, QStringLiteral("dropMaterialRate"), 1.0f));
            definition.dropEquipmentRate = qMax(0.0f, readFloatProperty(objectElement, QStringLiteral("dropEquipmentRate"), 1.0f));
            definition.dropSetLevel = qMax(0, readIntProperty(objectElement, QStringLiteral("dropSetLevel"), 0));

            switch (definition.monsterTier) {
            case CombatBalance::MonsterTier::Boss:
                definition.attackIntervalMs = 780;
                definition.respawnMs = isPartyDungeonMap(mapId) ? 0 : 15000;
                break;
            case CombatBalance::MonsterTier::Elite:
                definition.attackIntervalMs = 860;
                definition.respawnMs = isPartyDungeonMap(mapId) ? 0 : 9000;
                break;
            case CombatBalance::MonsterTier::Normal:
                definition.attackIntervalMs = 960;
                definition.respawnMs = isPartyDungeonMap(mapId) ? 0 : 5000;
                break;
            }

            const int respawnSec = readIntProperty(objectElement, QStringLiteral("respawnSec"), 0);
            const int respawnMs = readIntProperty(objectElement, QStringLiteral("respawnMs"), 0);
            if (respawnMs > 0 || respawnSec > 0) {
                definition.respawnMs = qMax(0, respawnMs > 0 ? respawnMs : respawnSec * 1000);
            }

            definitions.push_back(definition);
        }
    }
    return definitions;
}

/**
 * @brief 解析resolveDungeonPartyLevelCeiling相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
int core::resolveDungeonPartyLevelCeiling(const QString& mapId, qint64 instanceId) const
{
    int highestLevel = 0;
    const QString normalizedMapId = mapId.trimmed().isEmpty() ? QStringLiteral("BornWorld") : mapId.trimmed();
    const qint64 normalizedInstanceId = qMax<qint64>(0, instanceId);
    for (const auto& trackedPlayer : players) {
        if (!isClientOnline(trackedPlayer.clientId)) {
            continue;
        }
        if (trackedPlayer.mapId.compare(normalizedMapId, Qt::CaseInsensitive) != 0
            || trackedPlayer.instanceId != normalizedInstanceId)
        {
            continue;
        }
        highestLevel = qMax(highestLevel, CombatBalance::clampLevel(trackedPlayer.level));
    }
    return highestLevel;
}

/**
 * @brief 解析resolveDungeonMonsterLevel相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
int core::resolveDungeonMonsterLevel(const DungeonRoomState& room, const MonsterSpawnDefinition& spawn) const
{
    if (isPartyDungeonMap(room.mapId)) {
        return qMax(1,
                    room.scaledLevel > 0
                        ? room.scaledLevel
                        : qMax(1, CombatBalance::recommendedLevelForMap(room.mapId)));
    }
    return qMax(1,
                spawn.monsterLevel > 0
                    ? spawn.monsterLevel
                    : CombatBalance::recommendedLevelForMap(room.mapId));
}

/**
 * @brief 刷新refreshDungeonRoomScaling相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::refreshDungeonRoomScaling(DungeonRoomState& room)
{
    if (!isPartyDungeonMap(room.mapId)) {
        room.scaledLevel = qMax(room.scaledLevel,
                                qMax(1, CombatBalance::recommendedLevelForMap(room.mapId)));
        return;
    }

    const int highestLevel = resolveDungeonPartyLevelCeiling(room.mapId, room.instanceId);
    const int targetLevel = qMax(room.scaledLevel, qMax(1, highestLevel));
    if (targetLevel <= 0 || targetLevel == room.scaledLevel) {
        return;
    }

    room.scaledLevel = targetLevel;
    for (int runtimeId : room.runtimeOrder) {
        if (!room.spawns.contains(runtimeId) || !room.monsters.contains(runtimeId)) {
            continue;
        }

        const MonsterSpawnDefinition& spawn = room.spawns[runtimeId];
        MonsterState& monster = room.monsters[runtimeId];
        const CombatBalance::MonsterStats scaledStats =
            CombatBalance::monsterStats(targetLevel, spawn.monsterTier);
        const int nextMaxHealth = qMax(1, scaledStats.maxHealth);
        const int nextAttack = qMax(0, scaledStats.attack);
        const qreal healthRatio = monster.maxHealth > 0
                                      ? qBound<qreal>(0.0,
                                                      static_cast<qreal>(monster.health) / monster.maxHealth,
                                                      1.0)
                                      : 1.0;

        monster.maxHealth = nextMaxHealth;
        monster.attack = nextAttack;
        if (monster.alive) {
            monster.health = qBound(1, qRound(nextMaxHealth * healthRatio), nextMaxHealth);
        } else {
            monster.health = 0;
        }
    }
    room.dirty = true;
}

/**
 * @brief 构造ensureDungeonRoom相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
DungeonRoomState* core::ensureDungeonRoom(const QString& mapId, qint64 instanceId)
{
    const QString key = roomKeyFor(mapId, instanceId);
    auto roomIt = m_dungeonRooms.find(key);
    if (roomIt != m_dungeonRooms.end()) {
        refreshDungeonRoomScaling(roomIt.value());
        return &roomIt.value();
    }

    const QVector<MonsterSpawnDefinition> definitions = loadMonsterSpawnDefinitions(mapId);
    if (definitions.isEmpty()) {
        return nullptr;
    }

    DungeonRoomState room;
    room.mapId = mapId.trimmed().isEmpty() ? QStringLiteral("BornWorld") : mapId.trimmed();
    room.instanceId = qMax<qint64>(0, instanceId);
    room.scaledLevel = qMax(1, resolveDungeonPartyLevelCeiling(room.mapId, room.instanceId));
    if (room.scaledLevel <= 0) {
        room.scaledLevel = qMax(1, CombatBalance::recommendedLevelForMap(room.mapId));
    }
    room.dirty = true;

    for (const MonsterSpawnDefinition& definition : definitions) {
        room.spawns.insert(definition.runtimeId, definition);
        room.runtimeOrder.push_back(definition.runtimeId);

        MonsterState state;
        state.runtimeId = definition.runtimeId;
        state.position = definition.spawnPos;
        const int resolvedMonsterLevel = resolveDungeonMonsterLevel(room, definition);
        if (isPartyDungeonMap(room.mapId)) {
            const CombatBalance::MonsterStats stats =
                CombatBalance::monsterStats(resolvedMonsterLevel, definition.monsterTier);
            state.health = qMax(1, stats.maxHealth);
            state.maxHealth = qMax(1, stats.maxHealth);
            state.attack = qMax(0, stats.attack);
        } else {
            state.health = definition.maxHealth;
            state.maxHealth = definition.maxHealth;
            state.attack = definition.attack;
        }
        state.alive = true;
        state.chasing = false;
        room.monsters.insert(definition.runtimeId, state);
    }

    room.totalMonsterCount = room.runtimeOrder.size();
    room.aliveMonsterCount = room.totalMonsterCount;
    refreshDungeonRoomScaling(room);

    m_dungeonRooms.insert(key, room);
    return &m_dungeonRooms[key];
}

/**
 * @brief 处理resetDungeonRoomIfNoActivePlayers相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::resetDungeonRoomIfNoActivePlayers(const QString& mapId, qint64 instanceId)
{
    const QString key = roomKeyFor(mapId, instanceId);
    auto roomIt = m_dungeonRooms.find(key);
    if (roomIt == m_dungeonRooms.end()) {
        return;
    }

    DungeonRoomState& room = roomIt.value();
    if (!collectScopedClientIds(mapId, instanceId).isEmpty()) {
        room.emptySinceMs = 0;
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (room.emptySinceMs <= 0) {
        room.emptySinceMs = nowMs;
    }

    if (!isPartyDungeonMap(room.mapId)
        || room.completed
        || (nowMs - room.emptySinceMs) >= kDungeonEmptyRoomPersistMs)
    {
        m_dungeonRooms.erase(roomIt);
    }
}

/**
 * @brief 刷新refreshDungeonRoomProgress相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool core::refreshDungeonRoomProgress(DungeonRoomState& room)
{
    const int previousTotal = room.totalMonsterCount;
    const int previousAlive = room.aliveMonsterCount;
    const bool previousCompleted = room.completed;
    const bool continuousRespawn = isContinuousRespawnMap(room.mapId);

    int totalCount = 0;
    int aliveCount = 0;
    for (int runtimeId : room.runtimeOrder) {
        if (!room.spawns.contains(runtimeId) || !room.monsters.contains(runtimeId)) {
            continue;
        }
        ++totalCount;
        if (room.monsters.value(runtimeId).alive) {
            ++aliveCount;
        }
    }

    room.totalMonsterCount = totalCount;
    room.aliveMonsterCount = aliveCount;
    room.completed = !continuousRespawn && totalCount > 0 && aliveCount == 0;
    if (!previousCompleted && room.completed) {
        room.clearVersion = static_cast<quint16>(room.clearVersion + 1);
    }

    return previousTotal != room.totalMonsterCount
           || previousAlive != room.aliveMonsterCount
           || previousCompleted != room.completed;
}

/**
 * @brief 处理settleDungeonRoom相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::settleDungeonRoom(DungeonRoomState& room)
{
    if (!isPartyDungeonMap(room.mapId)
        || room.clearVersion == 0
        || room.settledClearVersion == room.clearVersion)
    {
        return;
    }

    int primaryRewardPlayerId = 0;
    int partyId = 0;
    int leaderPlayerId = 0;
    const QVector<Player_Information*> scopedPlayers = collectScopedPlayers(room.mapId, room.instanceId);
    for (Player_Information* playerInfo : scopedPlayers) {
        if (!playerInfo) {
            continue;
        }
        if (const PartyRuntimeState* party = findPartyByPlayerId(playerInfo->player_UserId)) {
            primaryRewardPlayerId = playerInfo->player_UserId;
            partyId = party->partyId;
            leaderPlayerId = party->leaderPlayerId;
            break;
        }
    }
    if (primaryRewardPlayerId == 0 && !room.participantUpdatedAtMs.isEmpty()) {
        primaryRewardPlayerId = room.participantUpdatedAtMs.constBegin().key();
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const RewardParticipationRule settlementRule = serverRewardRules().dungeonSettlement;
    QVector<Player_Information*> rewardRecipients =
        collectEligibleRewardRecipients(room,
                                        primaryRewardPlayerId,
                                        room.participantUpdatedAtMs,
                                        nowMs,
                                        settlementRule.participationWindowMs,
                                        settlementRule.minimumDamage,
                                        settlementRule.minimumHitCount);
    if (rewardRecipients.isEmpty()) {
        return;
    }
    if (partyId == 0) {
        if (const PartyRuntimeState* party = findPartyByPlayerId(rewardRecipients.front()->player_UserId)) {
            partyId = party->partyId;
            leaderPlayerId = party->leaderPlayerId;
        }
    }

    const int memberCount = rewardRecipients.size();
    const int recommendedLevel = qMax(1,
                                      room.scaledLevel > 0
                                          ? room.scaledLevel
                                          : CombatBalance::recommendedLevelForMap(room.mapId));
    const int completionExpReward = qMax(28,
                                         qRound(recommendedLevel * 18.0
                                                + room.totalMonsterCount * 26.0
                                                + memberCount * 12.0));

    for (Player_Information* trackedPlayer : rewardRecipients) {
        if (!trackedPlayer) {
            continue;
        }

        const Player_Information previousSettlementState = *trackedPlayer;
        Player_Information updatedSettlementState = previousSettlementState;
        bool leveledUp = false;
        applyServerExperienceReward(updatedSettlementState, completionExpReward, &leveledUp);
        const QVector<QPair<int, int>> plannedDrops =
            serverGenerateDungeonSettlementDrops(room.mapId,
                                                 recommendedLevel,
                                                 room.totalMonsterCount,
                                                 memberCount);
        const QVector<QPair<int, int>> acceptedDrops =
            applyDropsToTrackedBag(updatedSettlementState,
                                   plannedDrops,
                                   QDateTime::currentMSecsSinceEpoch());
        updatedSettlementState.inventoryStateVersion =
            previousSettlementState.inventoryStateVersion + 1;

        QJsonObject settlementEvent;
        settlementEvent.insert(QStringLiteral("type"), QStringLiteral("dungeon_settlement"));
        settlementEvent.insert(QStringLiteral("mapId"), room.mapId);
        settlementEvent.insert(QStringLiteral("instanceId"), QString::number(room.instanceId));
        settlementEvent.insert(QStringLiteral("recommendedLevel"), recommendedLevel);
        settlementEvent.insert(QStringLiteral("completionExpReward"), completionExpReward);
        settlementEvent.insert(QStringLiteral("memberCount"), memberCount);
        settlementEvent.insert(QStringLiteral("leveledUp"), leveledUp);
        settlementEvent.insert(QStringLiteral("drops"),
                               inventoryJournalItemEntriesToJson(acceptedDrops));
        QString stageError;
        if (!stageInventoryPersistence(updatedSettlementState,
                                       QStringLiteral("dungeon_settlement"),
                                       QString::fromUtf8(
                                           QJsonDocument(settlementEvent).toJson(QJsonDocument::Compact)),
                                       &stageError))
        {
            qWarning() << "Failed to stage dungeon settlement persistence:"
                       << trackedPlayer->player_UserId
                       << stageError;
            continue;
        }

        *trackedPlayer = updatedSettlementState;

        STRU_DUNGEON_SETTLEMENT_RS rs{};
        rs.recipientPlayerId = trackedPlayer->player_UserId;
        rs.partyId = partyId;
        rs.leaderPlayerId = leaderPlayerId;
        std::strncpy(rs.mapId, room.mapId.toUtf8().constData(), sizeof(rs.mapId) - 1);
        rs.instanceId = room.instanceId;
        rs.totalMonsterCount = room.totalMonsterCount;
        rs.defeatedMonsterCount = qMax(0, room.totalMonsterCount - room.aliveMonsterCount);
        rs.completionExpReward = completionExpReward;
        rs.newExperience = trackedPlayer->exp;
        rs.newLevel = trackedPlayer->level;
        rs.health = trackedPlayer->health;
        rs.mana = trackedPlayer->mana;
        rs.attackPower = trackedPlayer->attackPower;
        rs.magicAttack = trackedPlayer->magicAttack;
        rs.independentAttack = trackedPlayer->independentAttack;
        rs.defence = trackedPlayer->defense;
        rs.magicDefence = trackedPlayer->magicDefense;
        rs.strength = trackedPlayer->strength;
        rs.intelligence = trackedPlayer->intelligence;
        rs.vitality = trackedPlayer->vitality;
        rs.spirit = trackedPlayer->spirit;
        rs.criticalRate = trackedPlayer->critRate;
        rs.magicCriticalRate = trackedPlayer->magicCritRate;
        rs.criticalDamage = trackedPlayer->critDamage;
        rs.attackSpeed = trackedPlayer->attackSpeed;
        rs.moveSpeed = trackedPlayer->moveSpeed;
        rs.castSpeed = trackedPlayer->castSpeed;
        rs.attackRange = qRound(trackedPlayer->attackRange);
        rs.settlementFlags = leveledUp ? _dungeon_settlement_flag_level_up : 0;
        rs.clearVersion = room.clearVersion;
        rs.memberCount = memberCount;
        rs.dropCount = qMin(acceptedDrops.size(), MAX_MONSTER_REWARD_ITEM_NUM);
        for (int i = 0; i < rs.dropCount; ++i) {
            rs.drops[i].itemId = acceptedDrops[i].first;
            rs.drops[i].count = acceptedDrops[i].second;
        }
        const QString message = QStringLiteral("队伍副本 %1 已通关，正在发放共享结算。").arg(room.mapId);
        std::strncpy(rs.message, message.toUtf8().constData(), sizeof(rs.message) - 1);

        const QByteArray packet = PacketBuilder::build(_default_protocol_dungeon_settlement_rs, rs);
        emit sendToClient(trackedPlayer->clientId, packet);
    }

    room.settledClearVersion = room.clearVersion;
}

/**
 * @brief 构建buildDungeonStatePacket相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
QByteArray core::buildDungeonStatePacket(const DungeonRoomState& room) const
{
    STRU_DUNGEON_STATE_RS rs{};
    std::strncpy(rs.mapId, room.mapId.toUtf8().constData(), sizeof(rs.mapId) - 1);
    rs.instanceId = room.instanceId;
    rs.totalMonsterCount = room.totalMonsterCount;
    rs.aliveMonsterCount = room.aliveMonsterCount;
    rs.defeatedMonsterCount = qMax(0, room.totalMonsterCount - room.aliveMonsterCount);
    rs.roomFlags = room.completed ? _dungeon_room_flag_completed : 0;
    rs.clearVersion = room.clearVersion;
    return PacketBuilder::build(_default_protocol_dungeon_state_rs, rs);
}

/**
 * @brief 构建buildDungeonRoomPacket相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
QByteArray core::buildDungeonRoomPacket(const DungeonRoomState& room) const
{
    STRU_MONSTER_SYNC_RS rs{};
    std::strncpy(rs.mapId, room.mapId.toUtf8().constData(), sizeof(rs.mapId) - 1);
    rs.instanceId = room.instanceId;

    int count = 0;
    for (int runtimeId : room.runtimeOrder) {
        if (count >= MAX_MONSTER_SYNC_NUM) {
            break;
        }
        const MonsterSpawnDefinition spawn = room.spawns.value(runtimeId);
        const MonsterState state = room.monsters.value(runtimeId);
        MonsterSyncState& syncState = rs.monsters[count++];
        syncState.runtimeId = runtimeId;
        std::strncpy(syncState.monsterName, spawn.displayName.toUtf8().constData(), sizeof(syncState.monsterName) - 1);
        syncState.x = static_cast<float>(state.position.x());
        syncState.y = static_cast<float>(state.position.y());
        syncState.npcType = 0;
        syncState.health = state.health;
        syncState.maxHealth = state.maxHealth;
        syncState.stateFlags = (state.alive ? _monster_state_alive : 0)
                               | (state.chasing ? _monster_state_chasing : 0);
    }
    rs.monsterCount = count;
    return PacketBuilder::build(_default_protocol_monster_sync_rs, rs);
}

/**
 * @brief 发送sendRealtimePacket相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::sendRealtimePacket(quint64 clientId, const QByteArray& packet)
{
    if (m_pKcpNet && m_pKcpNet->isKcpConnected(clientId)) {
        emit sendToClientKcp(clientId, packet);
    } else {
        emit sendToClient(clientId, packet);
    }
}

/**
 * @brief 发送sendDungeonRoomSnapshotPackets相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::sendDungeonRoomSnapshotPackets(const QVector<quint64>& clientIds,
                                          const QByteArray& roomPacket,
                                          const QByteArray& statePacket)
{
    for (quint64 clientId : clientIds) {
        sendRealtimePacket(clientId, roomPacket);
        sendRealtimePacket(clientId, statePacket);
    }
}

/**
 * @brief 同步syncDungeonStateToClient相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::syncDungeonStateToClient(quint64 clientId, const QString& mapId, qint64 instanceId)
{
    DungeonRoomState* room = ensureDungeonRoom(mapId, instanceId);
    if (!room) {
        return;
    }
    refreshDungeonRoomProgress(*room);
    sendRealtimePacket(clientId, buildDungeonStatePacket(*room));
}

/**
 * @brief 同步syncDungeonRoomToClient相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::syncDungeonRoomToClient(quint64 clientId, const QString& mapId, qint64 instanceId)
{
    DungeonRoomState* room = ensureDungeonRoom(mapId, instanceId);
    if (!room) {
        return;
    }
    refreshDungeonRoomProgress(*room);
    const QVector<quint64> singleClient{clientId};
    sendDungeonRoomSnapshotPackets(singleClient,
                                   buildDungeonRoomPacket(*room),
                                   buildDungeonStatePacket(*room));
}

/**
 * @brief 广播broadcastDungeonRoomSnapshot相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::broadcastDungeonRoomSnapshot(const QString& mapId, qint64 instanceId)
{
    DungeonRoomState* room = ensureDungeonRoom(mapId, instanceId);
    if (!room) {
        return;
    }
    refreshDungeonRoomProgress(*room);
    const QVector<quint64> scopedClients = collectScopedClientIds(mapId, instanceId);
    if (scopedClients.isEmpty()) {
        return;
    }
    sendDungeonRoomSnapshotPackets(scopedClients,
                                   buildDungeonRoomPacket(*room),
                                   buildDungeonStatePacket(*room));
}

/**
 * @brief 计算computeMonsterDamageAgainstPlayer相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
int core::computeMonsterDamageAgainstPlayer(const Player_Information& playerInfo,
                                            int rawAttack,
                                            int monsterLevel,
                                            CombatBalance::MonsterTier tier) const
{
    const float dodgeChance = qBound(0.01f,
                                     serverPlayerDodgeChance(playerInfo.level,
                                                             playerInfo.moveSpeed,
                                                             playerInfo.spirit)
                                         - serverMonsterHitChance(monsterLevel, tier) * 0.08f,
                                     0.26f);
    if (QRandomGenerator::global()->generateDouble() < dodgeChance) {
        return 0;
    }

    const int safeAttack = qMax(1, rawAttack);
    const int defence = qMax(0, playerInfo.defense + qRound(playerInfo.vitality * 0.46f));
    const double randomFactor = 0.92 + QRandomGenerator::global()->generateDouble() * 0.22;
    const qreal rolledAttack = qMax<qreal>(1.0, safeAttack * randomFactor);
    const qreal mitigated = rolledAttack * (1.0 - serverDefenceReductionRatio(defence));
    const int floorDamage = qMax(1, qRound(safeAttack * 0.06));
    return qMax(floorDamage, qRound(mitigated));
}

/**
 * @brief 更新tickDungeonRooms相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::tickDungeonRooms()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    pruneExpiredSkillHitWindows(nowMs);
    QVector<QString> emptyRoomKeys;

    for (auto roomIt = m_dungeonRooms.begin(); roomIt != m_dungeonRooms.end(); ++roomIt) {
        DungeonRoomState& room = roomIt.value();
        const QVector<quint64> scopedClientIds = collectScopedClientIds(room.mapId, room.instanceId);
        if (scopedClientIds.isEmpty()) {
            if (room.emptySinceMs <= 0) {
                room.emptySinceMs = nowMs;
            }
            if (!isPartyDungeonMap(room.mapId)
                || room.completed
                || (nowMs - room.emptySinceMs) >= kDungeonEmptyRoomPersistMs)
            {
                emptyRoomKeys.push_back(roomIt.key());
            }
            continue;
        }
        room.emptySinceMs = 0;
        refreshDungeonRoomScaling(room);

        QVector<Player_Information*> scopedPlayers = collectScopedPlayers(room.mapId, room.instanceId);
        scopedPlayers.erase(std::remove_if(scopedPlayers.begin(),
                                           scopedPlayers.end(),
                                           [](Player_Information* playerInfo) {
                                               return !playerInfo || playerInfo->health <= 0;
                                           }),
                            scopedPlayers.end());

        for (int runtimeId : room.runtimeOrder) {
            if (!room.monsters.contains(runtimeId) || !room.spawns.contains(runtimeId)) {
                continue;
            }

            MonsterState& monster = room.monsters[runtimeId];
            const MonsterSpawnDefinition spawn = room.spawns.value(runtimeId);

            if (!monster.alive) {
                if (room.completed && !isContinuousRespawnMap(room.mapId)) {
                    continue;
                }
                if (spawn.respawnMs > 0 && nowMs >= monster.respawnAtMs) {
                    monster.position = spawn.spawnPos;
                    const int resolvedLevel = resolveDungeonMonsterLevel(room, spawn);
                    if (isPartyDungeonMap(room.mapId)) {
                        const CombatBalance::MonsterStats stats =
                            CombatBalance::monsterStats(resolvedLevel, spawn.monsterTier);
                        monster.health = qMax(1, stats.maxHealth);
                        monster.maxHealth = qMax(1, stats.maxHealth);
                        monster.attack = qMax(0, stats.attack);
                    } else {
                        monster.health = spawn.maxHealth;
                        monster.maxHealth = spawn.maxHealth;
                        monster.attack = spawn.attack;
                    }
                    monster.alive = true;
                    monster.chasing = false;
                    monster.nextAttackAllowedMs = 0;
                    monster.contributorUpdatedAtMs.clear();
                    room.dirty = true;
                }
                continue;
            }

            if (spawn.moveSpeed <= 0.0f || spawn.aggroRange <= 0.0f) {
                continue;
            }

            Player_Information* targetPlayer = nullptr;
            qreal nearestDistance = std::numeric_limits<qreal>::max();
            const QPointF monsterCenter = monster.position + QPointF(kServerNpcSize / 2.0, kServerNpcSize / 2.0);
            for (Player_Information* playerInfo : scopedPlayers) {
                const QPointF playerCenter(playerInfo->positionX + kServerPlayerSize / 2.0,
                                           playerInfo->positionY + kServerPlayerSize / 2.0);
                const qreal distance = QLineF(monsterCenter, playerCenter).length();
                if (distance <= spawn.aggroRange && distance < nearestDistance) {
                    nearestDistance = distance;
                    targetPlayer = playerInfo;
                }
            }

            QPointF desiredPosition = monster.position;
            if (targetPlayer) {
                monster.chasing = true;
                const QPointF playerPos(targetPlayer->positionX, targetPlayer->positionY);
                const qreal attackDistance = qMax<qreal>(18.0, spawn.attackRange);
                if (nearestDistance <= attackDistance) {
                    if (nowMs >= monster.nextAttackAllowedMs) {
                        const int damage = computeMonsterDamageAgainstPlayer(*targetPlayer,
                                                                            monster.attack,
                                                                            resolveDungeonMonsterLevel(room, spawn),
                                                                            spawn.monsterTier);
                        monster.nextAttackAllowedMs = nowMs
                                                     + qMax(420, spawn.attackIntervalMs)
                                                     + QRandomGenerator::global()->bounded(0, 120);

                        if (damage > 0) {
                            targetPlayer->health = qMax(0, targetPlayer->health - damage);
                            if (auto infoIt = m_mapPlayerInfo.find(targetPlayer->player_UserId);
                                infoIt != m_mapPlayerInfo.end() && infoIt->second)
                            {
                                infoIt->second->health = targetPlayer->health;
                            }

                            STRU_MONSTER_ATTACK_RS attackRs{};
                            attackRs.monsterRuntimeId = monster.runtimeId;
                            attackRs.targetPlayerId = targetPlayer->player_UserId;
                            attackRs.damage = damage;
                            attackRs.currentHealth = targetPlayer->health;
                            attackRs.isDead = targetPlayer->health <= 0;
                            const QByteArray packet = PacketBuilder::build(_default_protocol_monster_attack_rs, attackRs);
                            emit sendToClient(targetPlayer->clientId, packet);
                        }
                    }
                    continue;
                }

                const qreal distToSpawn = QLineF(monster.position, spawn.spawnPos).length();
                if (distToSpawn <= spawn.leashRange + 1.0f) {
                    QLineF moveLine(monster.position, playerPos);
                    const qreal originalDistance = moveLine.length();
                    if (originalDistance >= 0.8) {
                        moveLine.setLength(qMin<qreal>(spawn.moveSpeed, originalDistance));
                        desiredPosition = moveLine.p2();
                    }
                } else {
                    monster.chasing = false;
                }
            } else {
                monster.chasing = false;
                QLineF returnLine(monster.position, spawn.spawnPos);
                const qreal originalDistance = returnLine.length();
                if (originalDistance >= 0.8) {
                    returnLine.setLength(qMin<qreal>(spawn.moveSpeed, originalDistance));
                    desiredPosition = returnLine.p2();
                }
            }

            if (QLineF(monster.position, desiredPosition).length() > 0.5) {
                monster.position = desiredPosition;
                room.dirty = true;
            }
        }

        const bool completionBeforeRefresh = room.completed;
        if (refreshDungeonRoomProgress(room)) {
            room.dirty = true;
        }
        if (!completionBeforeRefresh && room.completed) {
            settleDungeonRoom(room);
        }

        if (room.dirty || (nowMs - room.lastBroadcastMs) >= 450) {
            room.lastBroadcastMs = nowMs;
            room.dirty = false;
            sendDungeonRoomSnapshotPackets(scopedClientIds,
                                           buildDungeonRoomPacket(room),
                                           buildDungeonStatePacket(room));
        }
    }

    for (const QString& key : emptyRoomKeys) {
        m_dungeonRooms.remove(key);
    }
}

/**
 * @brief 处理ensureEquipmentColumns相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool core::ensureEquipmentColumns()
{
    CMySql sql;
    const std::vector<std::pair<const char*, const char*>> columns = {
        {"u_equipment_weapon", "INT NOT NULL DEFAULT 0"},
        {"u_equipment_head", "INT NOT NULL DEFAULT 0"},
        {"u_equipment_body", "INT NOT NULL DEFAULT 0"},
        {"u_equipment_legs", "INT NOT NULL DEFAULT 0"},
        {"u_equipment_hands", "INT NOT NULL DEFAULT 0"},
        {"u_equipment_shoes", "INT NOT NULL DEFAULT 0"},
        {"u_equipment_shield", "INT NOT NULL DEFAULT 0"}
    };

    for (const auto& column : columns) {
        if (!ensureSchemaColumnExists(sql,
                                      QStringLiteral("user_basic_information"),
                                      QString::fromLatin1(column.first),
                                      QString::fromLatin1(column.second)))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 处理ensurePlayerStatColumns相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool core::ensurePlayerStatColumns()
{
    CMySql sql;
    const std::vector<std::pair<const char*, const char*>> columns = {
        {"u_mana", "INT NOT NULL DEFAULT 0"},
        {"u_magicattack", "INT NOT NULL DEFAULT 0"},
        {"u_independentattack", "INT NOT NULL DEFAULT 0"},
        {"u_magicdefence", "INT NOT NULL DEFAULT 0"},
        {"u_strength", "INT NOT NULL DEFAULT 0"},
        {"u_intelligence", "INT NOT NULL DEFAULT 0"},
        {"u_vitality", "INT NOT NULL DEFAULT 0"},
        {"u_spirit", "INT NOT NULL DEFAULT 0"},
        {"u_magiccritrate", "FLOAT NOT NULL DEFAULT 0"},
        {"u_attackspeed", "FLOAT NOT NULL DEFAULT 0"},
        {"u_movespeed", "FLOAT NOT NULL DEFAULT 0"},
        {"u_castspeed", "FLOAT NOT NULL DEFAULT 0"}
    };

    for (const auto& column : columns) {
        if (!ensureSchemaColumnExists(sql,
                                      QStringLiteral("user_basic_information"),
                                      QString::fromLatin1(column.first),
                                      QString::fromLatin1(column.second)))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 处理ensureEquipmentStateTable相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool core::ensureEquipmentStateTable()
{
    CMySql sql;
    const char* createSql =
        "CREATE TABLE IF NOT EXISTS user_equipment_state ("
        "u_id INT NOT NULL, "
        "slot_index INT NOT NULL, "
        "item_id INT NOT NULL DEFAULT 0, "
        "enhance_level INT NOT NULL DEFAULT 0, "
        "forge_level INT NOT NULL DEFAULT 0, "
        "enchant_kind INT NOT NULL DEFAULT 0, "
        "enchant_value INT NOT NULL DEFAULT 0, "
        "enhance_success_count INT NOT NULL DEFAULT 0, "
        "forge_success_count INT NOT NULL DEFAULT 0, "
        "enchant_success_count INT NOT NULL DEFAULT 0, "
        "PRIMARY KEY (u_id, slot_index)"
        ");";
    if (!sql.UpdateMySql(createSql)) {
        return false;
    }

    struct EquipmentStateColumnDef
    {
        const char* name;
        const char* definition;
    };

    const EquipmentStateColumnDef columns[] = {
        {"enhance_success_count", "INT NOT NULL DEFAULT 0"},
        {"forge_success_count", "INT NOT NULL DEFAULT 0"},
        {"enchant_success_count", "INT NOT NULL DEFAULT 0"}
    };

    for (const auto& column : columns) {
        if (!ensureSchemaColumnExists(sql,
                                      QStringLiteral("user_equipment_state"),
                                      QString::fromLatin1(column.name),
                                      QString::fromLatin1(column.definition)))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 处理ensureProgressStateTable相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool core::ensureProgressStateTable()
{
    CMySql sql;
    const char* createSql =
        "CREATE TABLE IF NOT EXISTS user_progress_state ("
        "u_id INT NOT NULL PRIMARY KEY, "
        "map_id VARCHAR(64) NOT NULL DEFAULT 'BornWorld', "
        "quest_step INT NOT NULL DEFAULT 0, "
        "warehouse_unlock_tier INT NOT NULL DEFAULT 1, "
        "pos_x FLOAT NOT NULL DEFAULT 100, "
        "pos_y FLOAT NOT NULL DEFAULT 100, "
        "updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ");";
    if (!sql.UpdateMySql(createSql)) {
        return false;
    }

    return ensureSchemaColumnExists(sql,
                                    QStringLiteral("user_progress_state"),
                                    QStringLiteral("warehouse_unlock_tier"),
                                    QStringLiteral("INT NOT NULL DEFAULT 1"));
}

/**
 * @brief 处理ensureWarehouseStateTable相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool core::ensureWarehouseStateTable()
{
    CMySql sql;
    const char* createSql =
        "CREATE TABLE IF NOT EXISTS user_warehouse_item ("
        "u_id INT NOT NULL, "
        "item_id INT NOT NULL, "
        "item_count INT NOT NULL DEFAULT 0, "
        "PRIMARY KEY (u_id, item_id)"
        ");";
    return sql.UpdateMySql(createSql);
}

bool core::ensureInventoryJournalTable()
{
    CMySql sql;
    const char* createSql =
        "CREATE TABLE IF NOT EXISTS user_inventory_journal ("
        "journal_id BIGSERIAL PRIMARY KEY, "
        "u_id INT NOT NULL, "
        "state_version INT NOT NULL DEFAULT 0, "
        "action_key VARCHAR(64) NOT NULL, "
        "payload_json TEXT NOT NULL, "
        "status VARCHAR(16) NOT NULL DEFAULT 'pending', "
        "attempt_count INT NOT NULL DEFAULT 0, "
        "last_error VARCHAR(255) NULL, "
        "applied_at TIMESTAMPTZ NULL DEFAULT NULL, "
        "created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ");";
    if (!sql.UpdateMySql(createSql)) {
        return false;
    }

    if (!ensureSchemaColumnExists(sql,
                                  QStringLiteral("user_inventory_journal"),
                                  QStringLiteral("status"),
                                  QStringLiteral("VARCHAR(16) NOT NULL DEFAULT 'applied'"))) {
        return false;
    }
    if (!ensureSchemaColumnExists(sql,
                                  QStringLiteral("user_inventory_journal"),
                                  QStringLiteral("attempt_count"),
                                  QStringLiteral("INT NOT NULL DEFAULT 0"))) {
        return false;
    }
    if (!ensureSchemaColumnExists(sql,
                                  QStringLiteral("user_inventory_journal"),
                                  QStringLiteral("last_error"),
                                  QStringLiteral("VARCHAR(255) NULL"))) {
        return false;
    }
    if (!ensureSchemaColumnExists(sql,
                                  QStringLiteral("user_inventory_journal"),
                                  QStringLiteral("applied_at"),
                                  QStringLiteral("TIMESTAMPTZ NULL DEFAULT NULL"))) {
        return false;
    }
    if (!ensureSchemaIndexExists(sql,
                                 QStringLiteral("user_inventory_journal"),
                                 QStringLiteral("idx_inventory_journal_status"),
                                 QStringLiteral("(status, created_at)"))) {
        return false;
    }
    updateQuery(sql,
                QStringLiteral("UPDATE user_inventory_journal "
                               "SET applied_at = created_at "
                               "WHERE status = 'applied' AND applied_at IS NULL;"));
    return true;
}

InventoryJournalVersionState core::loadInventoryJournalVersionState(int playerId) const
{
    InventoryJournalVersionState versionState;
    if (playerId <= 0) {
        return versionState;
    }

    CMySql sql;
    std::list<std::string> result;
    if (selectPreparedQuery(sql,
                            "SELECT COALESCE(MAX(state_version), 0), "
                            "COALESCE(MAX(CASE WHEN status = 'applied' THEN state_version ELSE 0 END), 0) "
                            "FROM user_inventory_journal WHERE u_id = ?;",
                            sqlParams(playerId),
                            2,
                            result)
        && result.size() >= 2)
    {
        try {
            versionState.latestKnownVersion =
                static_cast<quint32>(qMax(0, std::stoi(result.front())));
        } catch (...) {
            versionState.latestKnownVersion = 0;
        }
        result.pop_front();
        try {
            versionState.latestAppliedVersion =
                static_cast<quint32>(qMax(0, std::stoi(result.front())));
        } catch (...) {
            versionState.latestAppliedVersion = versionState.latestKnownVersion;
        }
    }

    versionState.latestAppliedVersion =
        qMin(versionState.latestAppliedVersion, versionState.latestKnownVersion);
    return versionState;
}

void core::primeTrackedPlayerInventoryVersion(Player_Information& playerInfo)
{
    const InventoryJournalVersionState versionState =
        loadInventoryJournalVersionState(playerInfo.player_UserId);
    playerInfo.inventoryStateVersion = versionState.latestKnownVersion;
    playerInfo.lastPersistedInventoryStateVersion = versionState.latestAppliedVersion;
}

bool core::stageInventoryPersistence(const Player_Information& playerInfo,
                                     const QString& action,
                                     const QString& eventPayloadJson,
                                     QString* errorMessage)
{
    if (playerInfo.player_UserId <= 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无效的角色编号，无法写入持久化队列。");
        }
        return false;
    }

    const QString snapshotPayload =
        buildInventorySnapshotJournalPayload(playerInfo, action, eventPayloadJson);
    CMySql sql;
    quint64 journalId = 0;
    if (!appendPendingInventoryJournalEntry(sql,
                                            playerInfo.player_UserId,
                                            playerInfo.inventoryStateVersion,
                                            action,
                                            snapshotPayload,
                                            &journalId))
    {
        if (errorMessage) {
            *errorMessage = QStringLiteral("库存 journal 落地失败，已拒绝本次状态变更。");
        }
        return false;
    }

    enqueueInventoryPersistence(playerInfo, action, snapshotPayload, journalId);
    return true;
}

void core::enqueueInventoryPersistence(const Player_Information& playerInfo,
                                       const QString& action,
                                       const QString& payloadJson,
                                       quint64 journalId)
{
    if (playerInfo.player_UserId <= 0) {
        return;
    }

    InventoryPersistenceJob job;
    job.journalId = journalId;
    job.playerId = playerInfo.player_UserId;
    job.version = playerInfo.inventoryStateVersion;
    job.action = action;
    job.payloadJson = payloadJson;
    job.snapshot = std::make_shared<Player_Information>(playerInfo);
    m_inventoryPersistQueues[job.playerId].push_back(job);

    if (m_inventoryPersistTimer && !m_inventoryPersistTimer->isActive()) {
        m_inventoryPersistTimer->start(0);
    }
}

void core::loadPendingInventoryPersistenceJobs()
{
    if (!ensureInventoryJournalTable()) {
        return;
    }

    CMySql sql;
    std::list<std::string> rows;
    if (!selectPreparedQuery(sql,
                             "SELECT journal_id, u_id, state_version, action_key, payload_json, attempt_count "
                             "FROM user_inventory_journal "
                             "WHERE status = ? "
                             "ORDER BY u_id ASC, state_version ASC, journal_id ASC;",
                             sqlParams(utf8StdString(QString::fromUtf8(kInventoryJournalStatusPending))),
                             6,
                             rows))
    {
        qWarning() << "Failed to load pending inventory journal rows during startup.";
        return;
    }

    while (rows.size() >= 6) {
        quint64 journalId = 0;
        int playerId = 0;
        quint32 version = 0;
        QString action;
        QString payloadJson;
        int attempt = 0;

        try {
            journalId = static_cast<quint64>(std::stoull(rows.front()));
        } catch (...) {
            journalId = 0;
        }
        rows.pop_front();

        try {
            playerId = std::stoi(rows.front());
        } catch (...) {
            playerId = 0;
        }
        rows.pop_front();

        try {
            version = static_cast<quint32>(qMax(0, std::stoi(rows.front())));
        } catch (...) {
            version = 0;
        }
        rows.pop_front();

        action = QString::fromStdString(rows.front()).trimmed();
        rows.pop_front();

        payloadJson = QString::fromStdString(rows.front());
        rows.pop_front();

        try {
            attempt = qMax(0, std::stoi(rows.front()));
        } catch (...) {
            attempt = 0;
        }
        rows.pop_front();

        Player_Information snapshot(0, QString(), 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.5f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f);
        if (!parseInventorySnapshotJournalPayload(payloadJson, &snapshot, &action)) {
            qWarning() << "Discarding unreadable pending inventory journal payload:"
                       << journalId << playerId << version;
            markInventoryJournalFailed(journalId, attempt + 1, QStringLiteral("invalid snapshot payload"));
            continue;
        }

        InventoryPersistenceJob job;
        job.journalId = journalId;
        job.playerId = snapshot.player_UserId;
        job.version = snapshot.inventoryStateVersion > 0
                          ? snapshot.inventoryStateVersion
                          : version;
        job.action = action;
        job.payloadJson = payloadJson;
        job.snapshot = std::make_shared<Player_Information>(snapshot);
        job.attempt = attempt;
        m_inventoryPersistQueues[job.playerId].push_back(job);
    }

    if (m_inventoryPersistTimer && !m_inventoryPersistQueues.isEmpty() && !m_inventoryPersistTimer->isActive()) {
        m_inventoryPersistTimer->start(0);
    }
}

int core::nextInventoryPersistencePlayerId()
{
    QList<int> playerIds = m_inventoryPersistQueues.keys();
    std::sort(playerIds.begin(), playerIds.end());
    if (playerIds.isEmpty()) {
        return 0;
    }

    int startIndex = 0;
    const int lastIndex = playerIds.indexOf(m_lastInventoryPersistPlayerId);
    if (lastIndex >= 0) {
        startIndex = (lastIndex + 1) % playerIds.size();
    }

    for (int offset = 0; offset < playerIds.size(); ++offset) {
        const int playerId = playerIds[(startIndex + offset) % playerIds.size()];
        if (m_inventoryPersistQueues.value(playerId).isEmpty()) {
            continue;
        }
        m_lastInventoryPersistPlayerId = playerId;
        return playerId;
    }
    return 0;
}

void core::processInventoryPersistenceQueue()
{
    const int playerId = nextInventoryPersistencePlayerId();
    if (playerId <= 0) {
        return;
    }

    auto queueIt = m_inventoryPersistQueues.find(playerId);
    if (queueIt == m_inventoryPersistQueues.end() || queueIt->isEmpty()) {
        m_inventoryPersistQueues.remove(playerId);
        if (m_inventoryPersistTimer && !m_inventoryPersistQueues.isEmpty()) {
            m_inventoryPersistTimer->start(0);
        }
        return;
    }

    if (queueIt->size() > 1) {
        const InventoryPersistenceJob latestJob = queueIt->last();
        for (int index = 0; index < queueIt->size() - 1; ++index) {
            const InventoryPersistenceJob& supersededJob = queueIt->at(index);
            if (supersededJob.journalId > 0) {
                CMySql markSql;
                if (!markInventoryJournalSuperseded(markSql, supersededJob.journalId)) {
                    qWarning() << "Failed to mark superseded inventory journal row:"
                               << supersededJob.journalId << supersededJob.playerId << supersededJob.version;
                }
            }
        }
        queueIt->clear();
        queueIt->push_back(latestJob);
    }

    InventoryPersistenceJob& job = queueIt->first();
    job.attempt += 1;

    bool savedOk = false;
    QString failureReason = QStringLiteral("inventory persistence transaction failed");
    CMySql sql;
    if (!job.snapshot) {
        failureReason = QStringLiteral("missing snapshot for persistence job");
    } else if (job.journalId == 0) {
        failureReason = QStringLiteral("missing journal id for persistence job");
    } else if (!sql.BeginTransaction()) {
        failureReason = QStringLiteral("begin transaction failed");
    } else {
        savedOk = persistTrackedPlayerInventoryState(sql, *job.snapshot)
                  && markInventoryJournalApplied(sql, job.journalId, job.attempt)
                  && sql.CommitTransaction();
        if (!savedOk) {
            sql.RollbackTransaction();
        }
    }

    if (savedOk) {
        if (Player_Information* trackedPlayer = findTrackedPlayer(job.playerId)) {
            trackedPlayer->lastPersistedInventoryStateVersion =
                qMax(trackedPlayer->lastPersistedInventoryStateVersion, job.version);
        }
        queueIt->removeFirst();
    } else {
        markInventoryJournalRetryPending(job.journalId, job.attempt, failureReason);
        qWarning() << "Inventory persistence retry scheduled for player"
                   << job.playerId << "version" << job.version << "attempt" << job.attempt;
    }

    if (queueIt->isEmpty()) {
        m_inventoryPersistQueues.erase(queueIt);
    }

    if (m_inventoryPersistTimer && !m_inventoryPersistQueues.isEmpty()) {
        m_inventoryPersistTimer->start(savedOk ? 0 : inventoryPersistenceRetryDelayMs(job.attempt));
    }
}

/**
 * @brief 关闭close相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::close(){
    //JaegerDebug();
    if (m_dungeonTickTimer) {
        m_dungeonTickTimer->stop();
    }
    if (m_inventoryPersistTimer) {
        m_inventoryPersistTimer->stop();
    }
    if (m_pTCPNet)
    {
        m_pTCPNet->unInitNetWork();
    }
    if (m_pKcpNet)
    {
        m_pKcpNet->unInitNetWork();
    }
    outFile.close();
}

/**
 * @brief 任务分发模块
 * @author Jaeger
 * @date 2025.3.28
 */
void core::dealData(quint64 clientId, const QByteArray& data)
{
    PacketHeader* header = (PacketHeader*)data.constData();
    const char* body = data.constData() + sizeof(PacketHeader);

    switch(header->cmd){
    case _default_protocol_test_rq:
        Test_Request(clientId,(STRU_TEST_RQ*)body);
        break;
    case _default_protocol_kcp_negotiate_rq:
        KcpNegotiate_Request(clientId, (STRU_KCP_NEGOTIATE_RQ*)body);
        break;
    case _default_protocol_login_rq:
        Login_Request(clientId,(STRU_LOGIN_RQ*)body);
        break;
    case _default_protocol_register_rq:
        Register_Request(clientId,(STRU_REGISTER_RQ*)body);
        break;
    case _default_protocol_initialize_rq:
        Initialize_Request(clientId,(STRU_INITIALIZE_RQ*)body);
        break;
    case _default_protocol_location_rq:
        Location_Request(clientId,(STRU_LOCATION_RQ*)body);
        break;
    case _default_protocol_chat_rq:
        Sendmessage_Request(clientId,(STRU_CHAT_RQ*)body);
        break;
    case _default_protocol_save_rq:
        Save_Request(clientId,(STRU_SAVE_RQ*)body);
        break;
    case _default_protocol_dazuo_rq:
        Dazuo_Request(clientId,(STRU_DAZUO_RQ*)body);
        break;
    case _default_protocol_attack_rq:
        HandleAttack(clientId,(STRU_ATTACK_RQ*)body);
        break;
    case _default_protocol_skillfx_rq:
        RelaySkillEffect(clientId, (STRU_SKILLFX_RQ*)body);
        break;
    case _default_protocol_monster_hit_rq:
        HandleMonsterHit(clientId, (STRU_MONSTER_HIT_RQ*)body);
        break;
    case _default_protocol_skillstate_rq:
        HandleSkillStateSync(clientId, (STRU_SKILLSTATE_RQ*)body);
        break;
    case _default_protocol_party_action_rq:
        HandlePartyAction(clientId, (STRU_PARTY_ACTION_RQ*)body);
        break;
    case _default_protocol_inventory_action_rq:
        HandleInventoryAction(clientId, (STRU_INVENTORY_ACTION_RQ*)body);
        break;
    case _default_protocol_initbag_rq:
        InitializeBag_Request(clientId,(STRU_INITBAG_RQ*)body);
        break;
    case _default_protocol_playerlist_rq:
        PlayerList_Request(clientId,(STRU_PLAYERLIST_RQ*)body);
        break;
    }
}

/**
 * @brief 处理Test_Request相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::Test_Request(quint64 clientId, STRU_TEST_RQ* rq)
{
    JaegerDebug();
    qDebug() << "ClientId:" << clientId;
    qDebug() << "PlayerName:" << rq->player_Name;
    qDebug() << "PlayerPassWord:(masked)";
    qDebug() << "PlayerTel:" << rq->player_tel;

    STRU_TEST_RS test_rs;

    bool ok =
        strlen(rq->player_Name) > 0 &&
        strlen(rq->player_Password) > 0 &&
        rq->player_tel > 0;

    test_rs.player_Result = ok ? 1 : 0;

    QByteArray packet = PacketBuilder::build(_default_protocol_test_rs,test_rs);
    emit sendToClient(clientId, packet);
}

/**
 * @brief 处理KcpNegotiate_Request相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::KcpNegotiate_Request(quint64 clientId, STRU_KCP_NEGOTIATE_RQ* rq)
{
    Q_UNUSED(rq);
    qDebug() << "KCP Negotiate Request from ClientID:" << clientId;

    STRU_KCP_NEGOTIATE_RS rs;
    rs.result = 0; // 默认失败
    rs.kcpConv = 0;
    rs.kcpPort = 0;
    rs.kcpMode = (quint8)QKcpSocket::Mode::Fast;

    QTcpSocket* tcpSocket = m_pTCPNet->getSocket(clientId);
    if (m_pKcpNet->handleKcpNegotiate(clientId, tcpSocket, rs.kcpConv, rs.kcpPort)) {
        rs.result = 1; // 成功
        qDebug() << "KCP Negotiate SUCCESS for ClientID:" << clientId << "Conv:" << rs.kcpConv << "Port:" << rs.kcpPort;
    } else {
        qDebug() << "KCP Negotiate FAILED for ClientID:" << clientId;
    }

    QByteArray sendDataArray = PacketBuilder::build(_default_protocol_kcp_negotiate_rs, rs);
    emit sendToClient(clientId, sendDataArray); // 通过TCP回包
}

/**
 * @brief 发送Sendmessage_Request相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::Sendmessage_Request(quint64 clientId, STRU_CHAT_RQ* rq){
    JaegerDebug();
    Q_UNUSED(clientId);
    STRU_CHAT_RS chat_rs;

    // 获取当前时间
    time_t now = time(nullptr);
    tm* localTime = localtime(&now);
    char timeStr[128];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localTime);

    // 复制玩家信息，防止溢出
    strncpy(chat_rs.player_Name, rq->player_Name, sizeof(chat_rs.player_Name) - 1);
    chat_rs.player_Name[sizeof(chat_rs.player_Name) - 1] = '\0';  // 确保字符串结尾
    strncpy(chat_rs.szbuf, rq->szbuf, sizeof(chat_rs.szbuf) - 1);
    chat_rs.szbuf[sizeof(chat_rs.szbuf) - 1] = '\0';  // 确保字符串结尾

    // 遍历 QMap 进行广播
    const auto& clientIds = m_pTCPNet->getAllClientIds();
    for (quint64 clientId : clientIds) {
        QByteArray packet = PacketBuilder::build(_default_protocol_chat_rs,chat_rs);
        emit sendToClient(clientId, packet);
    }

    // 记录聊天内容到日志文件
    std::cout << "[System Record] User " << chat_rs.player_Name << ": " << chat_rs.szbuf << std::endl;

    std::ofstream outFile("chat_history.txt", std::ios::app);  // 追加模式打开文件
    if (outFile.is_open()) {
        outFile << "[" << timeStr << "] User " << chat_rs.player_Name << ": " << chat_rs.szbuf << "\n";
        outFile.flush();
    } else {
        std::cerr << "Failed to open chat log file." << std::endl;
    }
}
/**
 * @brief 处理DoRegister相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
RegisterError core::DoRegister(STRU_REGISTER_RQ* rq)
{
    if (!rq) {
        return REG_INSERT_FAIL;
    }

    CMySql mysql;
    list<string> result;
    const int baseLevel = 1;
    const CombatBalance::PlayerStats baseStats = CombatBalance::playerStats(baseLevel);
    const QString rawUserName = QString::fromUtf8(rq->player_Name).trimmed();
    const long long rawTel = rq->player_tel;
    const QString passwordHash = hashPasswordForStorage(QString::fromUtf8(rq->player_Password));
    if (rawUserName.isEmpty()) {
        return REG_USERNAME_FAIL;
    }

    //查手机号
    if (!selectPreparedQuery(mysql,
                             "select u_id from user_account where u_tel = ?;",
                             sqlParams(rawTel),
                             1,
                             result))
        return REG_SELECT_USEREXIT_FAIL;

    if (!result.empty())
        return REG_USEREXIT_FAIL;

    result.clear();

    //查用户名
    if (!selectPreparedQuery(mysql,
                             "select u_id from user_account where u_name = ?;",
                             sqlParams(utf8StdString(rawUserName)),
                             1,
                             result))
        return REG_SELECT_USERNAME_FAIL;

    if (!result.empty())
        return REG_USERNAME_FAIL;

    if (!mysql.BeginTransaction()) {
        return REG_INSERT_FAIL;
    }

    auto rollbackAndReturn = [&](RegisterError err) -> RegisterError {
        mysql.RollbackTransaction();
        return err;
    };

    //插入用户
    if (!updatePreparedQuery(mysql,
                             "insert into user_account(u_name, u_password, u_tel) values(?, ?, ?) returning u_id;",
                             sqlParams(utf8StdString(rawUserName),
                                       utf8StdString(passwordHash),
                                       rawTel)))
        return rollbackAndReturn(REG_INSERT_FAIL);

    const unsigned long long insertedUserId = mysql.LastInsertId();
    if (insertedUserId == 0 ||
        insertedUserId > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
        return rollbackAndReturn(REG_INSERT_FAIL);
    }
    const int user_id = static_cast<int>(insertedUserId);

    if (!updatePreparedQuery(
            mysql,
            "insert into user_basic_information("
            "u_id, u_name, u_health, u_mana, u_attackpower, u_magicattack, u_independentattack, "
            "u_attackrange, u_experience, u_level, u_defence, u_magicdefence, "
            "u_strength, u_intelligence, u_vitality, u_spirit, "
            "u_critrate, u_magiccritrate, u_critdamage, "
            "u_attackspeed, u_movespeed, u_castspeed, u_position_x, u_position_y) "
            "values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
            sqlParams(user_id,
                      utf8StdString(rawUserName),
                      baseStats.maxHealth,
                      baseStats.maxMana,
                      baseStats.attack,
                      baseStats.magicAttack,
                      baseStats.independentAttack,
                      baseStats.attackRange,
                      0,
                      baseLevel,
                      baseStats.defence,
                      baseStats.magicDefence,
                      baseStats.strength,
                      baseStats.intelligence,
                      baseStats.vitality,
                      baseStats.spirit,
                      baseStats.critRate,
                      baseStats.magicCritRate,
                      baseStats.critDamage,
                      baseStats.attackSpeed,
                      baseStats.moveSpeed,
                      baseStats.castSpeed,
                      100.0,
                      100.0)))
        return rollbackAndReturn(REG_INSERT_FAIL);
    result.clear();

    const std::vector<std::pair<int, int>> starterItems = {
        {1, 1},      // 初行者长剑
        {7, 1},      // 初行者护盾
        {10, 1},     // 初行者胸甲
        {12, 1},     // 初行者头盔
        {13, 1},     // 初行者护腿
        {14, 1},     // 初行者护手
        {15, 1},     // 初行者战靴
        {8, 20},     // 红苹果
        {11, 8},     // 大红药水
        {1001, 45000}, // 金币
        {1002, 140}, // 强化石
        {1003, 90},  // 锻造锤
        {1004, 80},  // 魔力结晶
        {1005, 360}, // 无色晶块
        {1006, 130}  // 炉岩碳
    };

    for (const auto& item : starterItems) {
        if (!updatePreparedQuery(
                mysql,
                "INSERT INTO user_item (u_id, item_id, item_count) "
                "VALUES (?, ?, ?) "
                "ON CONFLICT (u_id, item_id) DO UPDATE SET "
                "item_count = user_item.item_count + EXCLUDED.item_count;",
                sqlParams(user_id, item.first, item.second))) {
            return rollbackAndReturn(REG_INSERT_FAIL);
        }
    }
    if (!mysql.CommitTransaction()) {
        mysql.RollbackTransaction();
        return REG_INSERT_FAIL;
    }
    return REG_OK;
}
/**
 * @brief 处理Register_Request相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::Register_Request(quint64 clientId, STRU_REGISTER_RQ* rq)
{
    JaegerDebug();

    STRU_REGISTER_RS register_rs;
    register_rs.player_Result = _register_err_;

    RegisterError err = DoRegister(rq);

    switch (err)
    {
    case REG_OK:
        register_rs.player_Result = _register_success_;
        break;

    case REG_USEREXIT_FAIL:        // 手机号已注册
        register_rs.player_Result = _register_userexist_;
        break;

    case REG_USERNAME_FAIL:        // 用户名已存在
        register_rs.player_Result = _register_nameexist_;
        break;

    case REG_INSERT_FAIL:
        register_rs.player_Result = _register_err_;
        break;

    case REG_SELECT_USEREXIT_FAIL:
        register_rs.player_Result = _register_err_;
        break;

    case REG_SELECT_USERNAME_FAIL:
        register_rs.player_Result = _register_err_;
        break;
    }

    if (err != REG_OK) {
        qWarning() << "Register failed for user"
                   << QString::fromUtf8(rq ? rq->player_Name : "")
                   << "with error code"
                   << err;
    } else {
        qDebug() << "Register success for user" << QString::fromUtf8(rq ? rq->player_Name : "");
    }
    QByteArray packet = PacketBuilder::build(_default_protocol_register_rs, register_rs);
    emit sendToClient(clientId, packet);
}

/**
 * @brief 处理Login_Request相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::Login_Request(quint64 clientId, STRU_LOGIN_RQ* rq)
{
    //JaegerDebug();

    CMySql mysql;
    STRU_LOGIN_RS login_rs{};
    list<string>liststr;
    login_rs.player_Result = _login_usernoexist;
    login_rs.player_UserId = -1;

    const QString rawUserName = QString::fromUtf8(rq->player_Name).trimmed();
    const QString rawPassword = QString::fromUtf8(rq->player_Password);
    if (!selectPreparedQuery(mysql,
                             "select u_id, u_password from user_account where u_name = ?;",
                             sqlParams(utf8StdString(rawUserName)),
                             2,
                             liststr)) {
        login_rs.player_Result = _login_error;
    } else if (!liststr.empty()) {
        login_rs.player_Result = _login_passworderr;
        const QString userIdText = QString::fromStdString(liststr.front());
        liststr.pop_front();
        const QString storedPassword = !liststr.empty()
                                           ? QString::fromStdString(liststr.front())
                                           : QString();
        bool ok = false;
        const int userId = userIdText.toInt(&ok);
        if (ok && verifyStoredPassword(rawPassword, storedPassword)) {
            login_rs.player_Result = _login_success;
            login_rs.player_UserId = userId;
            bindClientToPlayer(clientId, userId);

            if (!isStoredPasswordHashed(storedPassword)) {
                if (!updatePreparedQuery(mysql,
                                         "UPDATE user_account SET u_password = ? WHERE u_id = ?;",
                                         sqlParams(utf8StdString(hashPasswordForStorage(rawPassword)),
                                                   userId))) {
                    qWarning() << "Failed to upgrade password hash for user" << userId;
                }
            }
        }
    }
    if (login_rs.player_Result != _login_success) {
        clearClientBinding(clientId);
    }

    QByteArray packet = PacketBuilder::build(_default_protocol_login_rs,login_rs);
    emit sendToClient(clientId, packet);
}



/**
 * @brief 初始化Initialize_Request相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::Initialize_Request(quint64 clientId, STRU_INITIALIZE_RQ* rq)
{
    JaegerDebug();

    ensureProgressStateTable();
    CMySql sql;
    STRU_INITIALIZE_RS initialize_rs{};
    initialize_rs.Initialize_Result = _initialize_fail;
    snprintf(initialize_rs.mapId, sizeof(initialize_rs.mapId), "%s", "BornWorld");
    initialize_rs.instanceId = 0;
    initialize_rs.questStep = 0;
    if (!rq) {
        initialize_rs.Initialize_Result = _initialize_error;
        const QByteArray packet = PacketBuilder::build(_default_protocol_initialize_rs, initialize_rs);
        emit sendToClient(clientId, packet);
        return;
    }

    const int boundPlayerId = boundPlayerIdForClient(clientId);
    if (boundPlayerId <= 0 || boundPlayerId != rq->player_UserId) {
        qWarning() << "Initialize rejected because client binding mismatched:"
                   << clientId << rq->player_UserId << boundPlayerId;
        initialize_rs.Initialize_Result = _initialize_error;
        const QByteArray packet = PacketBuilder::build(_default_protocol_initialize_rs, initialize_rs);
        emit sendToClient(clientId, packet);
        return;
    }

    auto parseInt = [](const std::string& value, int fallback) -> int {
        try {
            return std::stoi(value);
        } catch (...) {
            return fallback;
        }
    };
    auto parseLongLong = [](const std::string& value, long long fallback) -> long long {
        try {
            return std::stoll(value);
        } catch (...) {
            return fallback;
        }
    };
    auto parseFloat = [](const std::string& value, float fallback) -> float {
        try {
            return std::stof(value);
        } catch (...) {
            return fallback;
        }
    };

    list<string> liststr;
    const QString initializeQuery =
        QStringLiteral("SELECT "
                       "b.u_name, b.u_health, b.u_mana, "
                       "b.u_attackpower, b.u_magicattack, b.u_independentattack, b.u_attackrange, "
                       "b.u_experience, b.u_level, "
                       "b.u_defence, b.u_magicdefence, "
                       "b.u_strength, b.u_intelligence, b.u_vitality, b.u_spirit, "
                       "b.u_critrate, b.u_magiccritrate, b.u_critdamage, "
                       "b.u_attackspeed, b.u_movespeed, b.u_castspeed, "
                       "b.u_position_x, b.u_position_y, "
                       "COALESCE(p.map_id, 'BornWorld'), COALESCE(p.quest_step, 0), "
                       "COALESCE(p.pos_x, b.u_position_x), COALESCE(p.pos_y, b.u_position_y) "
                       "FROM user_basic_information b "
                       "LEFT JOIN user_progress_state p ON p.u_id = b.u_id "
                       "WHERE b.u_id = ? LIMIT 1;");

    if (!selectPreparedQuery(sql, initializeQuery, sqlParams(boundPlayerId), 27, liststr)
        || liststr.size() < 27) {
        initialize_rs.Initialize_Result = _initialize_error;
        QByteArray packet = PacketBuilder::build(_default_protocol_initialize_rs, initialize_rs);
        emit sendToClient(clientId, packet);
        return;
    }

    string strUserName = liststr.front();
    snprintf(initialize_rs.player_Name, sizeof(initialize_rs.player_Name), "%s", strUserName.c_str());
    liststr.pop_front();

    initialize_rs.health = parseInt(liststr.front(), 100);
    liststr.pop_front();
    initialize_rs.mana = parseInt(liststr.front(), 60);
    liststr.pop_front();
    initialize_rs.attackPower = parseInt(liststr.front(), 10);
    liststr.pop_front();
    initialize_rs.magicAttack = parseInt(liststr.front(), 10);
    liststr.pop_front();
    initialize_rs.independentAttack = parseInt(liststr.front(), 10);
    liststr.pop_front();
    initialize_rs.attackRange = parseInt(liststr.front(), 30);
    liststr.pop_front();
    initialize_rs.experience = parseLongLong(liststr.front(), 0);
    liststr.pop_front();
    initialize_rs.level = parseInt(liststr.front(), 1);
    liststr.pop_front();
    initialize_rs.defence = parseInt(liststr.front(), 12);
    liststr.pop_front();
    initialize_rs.magicDefence = parseInt(liststr.front(), 10);
    liststr.pop_front();
    initialize_rs.strength = parseInt(liststr.front(), 0);
    liststr.pop_front();
    initialize_rs.intelligence = parseInt(liststr.front(), 0);
    liststr.pop_front();
    initialize_rs.vitality = parseInt(liststr.front(), 0);
    liststr.pop_front();
    initialize_rs.spirit = parseInt(liststr.front(), 0);
    liststr.pop_front();
    initialize_rs.critical_rate = parseFloat(liststr.front(), 0.05f);
    liststr.pop_front();
    initialize_rs.magic_critical_rate = parseFloat(liststr.front(), 0.04f);
    liststr.pop_front();
    initialize_rs.critical_damage = parseFloat(liststr.front(), 1.5f);
    liststr.pop_front();
    initialize_rs.attack_speed = parseFloat(liststr.front(), 0.0f);
    liststr.pop_front();
    initialize_rs.move_speed = parseFloat(liststr.front(), 0.0f);
    liststr.pop_front();
    initialize_rs.cast_speed = parseFloat(liststr.front(), 0.0f);
    liststr.pop_front();
    initialize_rs.x = parseFloat(liststr.front(), 100.0f);
    liststr.pop_front();
    initialize_rs.y = parseFloat(liststr.front(), 100.0f);
    liststr.pop_front();

    string strMapId = liststr.front();
    liststr.pop_front();
    if (strMapId.empty()) {
        strMapId = "BornWorld";
    }
    snprintf(initialize_rs.mapId, sizeof(initialize_rs.mapId), "%s", strMapId.c_str());

    initialize_rs.questStep = parseInt(liststr.front(), 0);
    liststr.pop_front();

    initialize_rs.x = parseFloat(liststr.front(), initialize_rs.x);
    liststr.pop_front();
    initialize_rs.y = parseFloat(liststr.front(), initialize_rs.y);
    liststr.pop_front();

    initialize_rs.level = CombatBalance::clampLevel(initialize_rs.level);
    initialize_rs.experience = qMax(0LL, initialize_rs.experience);

    // 统一按等级基准重建角色核心战斗属性，避免历史旧档造成数值漂移
    const CombatBalance::PlayerStats expected = CombatBalance::playerStats(initialize_rs.level);
    initialize_rs.attackPower = expected.attack;
    initialize_rs.magicAttack = expected.magicAttack;
    initialize_rs.independentAttack = expected.independentAttack;
    initialize_rs.defence = expected.defence;
    initialize_rs.magicDefence = expected.magicDefence;
    initialize_rs.strength = expected.strength;
    initialize_rs.intelligence = expected.intelligence;
    initialize_rs.vitality = expected.vitality;
    initialize_rs.spirit = expected.spirit;
    initialize_rs.critical_rate = expected.critRate;
    initialize_rs.magic_critical_rate = expected.magicCritRate;
    initialize_rs.critical_damage = expected.critDamage;
    initialize_rs.attack_speed = expected.attackSpeed;
    initialize_rs.move_speed = expected.moveSpeed;
    initialize_rs.cast_speed = expected.castSpeed;
    initialize_rs.attackRange = qMax(20, expected.attackRange);
    initialize_rs.health = qBound(1, initialize_rs.health, expected.maxHealth);
    initialize_rs.mana = qBound(0, initialize_rs.mana, expected.maxMana);

    // 将标准化后的属性同步回数据库（权威端）
    updateUserBasicInformation(sql,
                               boundPlayerId,
                               initialize_rs.health,
                               initialize_rs.mana,
                               initialize_rs.attackPower,
                               initialize_rs.magicAttack,
                               initialize_rs.independentAttack,
                               initialize_rs.attackRange,
                               initialize_rs.experience,
                               initialize_rs.level,
                               initialize_rs.defence,
                               initialize_rs.magicDefence,
                               initialize_rs.strength,
                               initialize_rs.intelligence,
                               initialize_rs.vitality,
                               initialize_rs.spirit,
                               initialize_rs.critical_rate,
                               initialize_rs.magic_critical_rate,
                               initialize_rs.critical_damage,
                               initialize_rs.attack_speed,
                               initialize_rs.move_speed,
                               initialize_rs.cast_speed);

    const QString initializedPlayerName = QString::fromUtf8(initialize_rs.player_Name).trimmed();
    const QString initializedMapId = QString::fromUtf8(initialize_rs.mapId).trimmed().isEmpty()
                                         ? QStringLiteral("BornWorld")
                                         : QString::fromUtf8(initialize_rs.mapId).trimmed();
    initialize_rs.instanceId =
        resolveAuthoritativeInstanceIdForPlayerMap(boundPlayerId, initializedMapId, initialize_rs.instanceId);
    if (Player_Information* trackedPlayer = findTrackedPlayer(boundPlayerId)) {
        trackedPlayer->clientId = clientId;
        trackedPlayer->playerName = initializedPlayerName;
        trackedPlayer->mapId = initializedMapId;
        trackedPlayer->instanceId = qMax<qint64>(0, initialize_rs.instanceId);
        trackedPlayer->level = initialize_rs.level;
        trackedPlayer->exp = initialize_rs.experience;
        trackedPlayer->startExp = initialize_rs.experience;
        trackedPlayer->health = initialize_rs.health;
        trackedPlayer->mana = initialize_rs.mana;
        trackedPlayer->attackPower = initialize_rs.attackPower;
        trackedPlayer->magicAttack = initialize_rs.magicAttack;
        trackedPlayer->independentAttack = initialize_rs.independentAttack;
        trackedPlayer->defense = initialize_rs.defence;
        trackedPlayer->magicDefense = initialize_rs.magicDefence;
        trackedPlayer->strength = initialize_rs.strength;
        trackedPlayer->intelligence = initialize_rs.intelligence;
        trackedPlayer->vitality = initialize_rs.vitality;
        trackedPlayer->spirit = initialize_rs.spirit;
        trackedPlayer->critRate = initialize_rs.critical_rate;
        trackedPlayer->magicCritRate = initialize_rs.magic_critical_rate;
        trackedPlayer->critDamage = initialize_rs.critical_damage;
        trackedPlayer->attackSpeed = initialize_rs.attack_speed;
        trackedPlayer->moveSpeed = initialize_rs.move_speed;
        trackedPlayer->castSpeed = initialize_rs.cast_speed;
        trackedPlayer->attackRange = initialize_rs.attackRange;
        trackedPlayer->positionX = initialize_rs.x;
        trackedPlayer->positionY = initialize_rs.y;
        trackedPlayer->direction = s;
        primeTrackedPlayerInventoryVersion(*trackedPlayer);
    } else {
        players.emplace_back(boundPlayerId,
                             initializedPlayerName,
                             initialize_rs.level,
                             initialize_rs.experience,
                             initialize_rs.experience,
                             initialize_rs.health,
                             initialize_rs.mana,
                             initialize_rs.attackPower,
                             initialize_rs.magicAttack,
                             initialize_rs.independentAttack,
                             initialize_rs.defence,
                             initialize_rs.magicDefence,
                             initialize_rs.strength,
                             initialize_rs.intelligence,
                             initialize_rs.vitality,
                             initialize_rs.spirit,
                             initialize_rs.critical_rate,
                             initialize_rs.magic_critical_rate,
                             initialize_rs.critical_damage,
                             initialize_rs.attack_speed,
                             initialize_rs.move_speed,
                             initialize_rs.cast_speed,
                             initialize_rs.attackRange,
                             initialize_rs.x,
                             initialize_rs.y,
                             clientId,
                             initializedMapId,
                             qMax<qint64>(0, initialize_rs.instanceId),
                             s);
        if (!players.empty()) {
            primeTrackedPlayerInventoryVersion(players.back());
        }
    }

    initialize_rs.Initialize_Result = _initialize_success;
    initialize_rs.player_UserId = boundPlayerId;
    qDebug() << "Init Success";
    QByteArray packet = PacketBuilder::build(_default_protocol_initialize_rs, initialize_rs);
    emit sendToClient(clientId, packet);
    sendPartyStateToPlayer(boundPlayerId, _party_action_query, 0, QString(), true);
}

/**
 * @brief 初始化InitializeBag_Request相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::InitializeBag_Request(quint64 clientId, STRU_INITBAG_RQ* rq)
{
    JaegerDebug();

    ensureEquipmentColumns();
    ensureEquipmentStateTable();
    ensureProgressStateTable();
    ensureWarehouseStateTable();
    CMySql sql;
    STRU_INITBAG_RS initbag_rs;
    memset(&initbag_rs, 0, sizeof(initbag_rs));
    if (!rq) {
        initbag_rs.Result = _initializebag_error;
        const QByteArray packet = PacketBuilder::build(_default_protocol_initbag_rs, initbag_rs);
        emit sendToClient(clientId, packet);
        return;
    }

    const int boundPlayerId = boundPlayerIdForClient(clientId);
    if (boundPlayerId <= 0 || (rq->playerId > 0 && rq->playerId != boundPlayerId)) {
        qWarning() << "InitializeBag rejected because client binding mismatched:"
                   << clientId << rq->playerId << boundPlayerId;
        initbag_rs.Result = _initializebag_error;
        const QByteArray packet = PacketBuilder::build(_default_protocol_initbag_rs, initbag_rs);
        emit sendToClient(clientId, packet);
        return;
    }

    std::vector<std::vector<std::string>> result;
    std::list<std::string> liststr;

    qDebug() << "InitBag: querying for player" << boundPlayerId;

    // 查询玩家物品
    const QString bagQuery =
        QStringLiteral("select item_id,item_count from user_item where u_id = ?");

    if(!selectPreparedQuery(sql, bagQuery, sqlParams(boundPlayerId), 2, liststr))
    {
        // 查询失败，直接返回错误
        initbag_rs.Result = _initializebag_error;
        QByteArray packet = PacketBuilder::build(_default_protocol_initbag_rs, initbag_rs);
        emit sendToClient(clientId, packet);
        return;
    }

    // 辅助函数：去除字符串两端空白字符
    auto trim = [](const std::string &s) -> std::string {
        size_t start = s.find_first_not_of(" \t\n\r");
        size_t end = s.find_last_not_of(" \t\n\r");
        return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
    };

    std::list<std::string> equipmentList;
    const QString equipmentQuery =
        QStringLiteral("select "
                       "u_equipment_weapon,"
                       "u_equipment_head,"
                       "u_equipment_body,"
                       "u_equipment_legs,"
                       "u_equipment_hands,"
                       "u_equipment_shoes,"
                       "u_equipment_shield "
                       "from user_basic_information where u_id = ?");

    if (selectPreparedQuery(sql,
                            equipmentQuery,
                            sqlParams(boundPlayerId),
                            MAX_EQUIPMENT_SLOT_NUM,
                            equipmentList)) {
        for (int i = 0; i < MAX_EQUIPMENT_SLOT_NUM && !equipmentList.empty(); ++i) {
            try {
                initbag_rs.equippedItemIds[i] = std::stoi(trim(equipmentList.front()));
            } catch (...) {
                initbag_rs.equippedItemIds[i] = 0;
            }
            equipmentList.pop_front();
        }
    }

    std::list<std::string> equipmentStateRows;
    const QString equipmentStateQuery =
        QStringLiteral("select slot_index,item_id,enhance_level,forge_level,enchant_kind,enchant_value,"
                       "enhance_success_count,forge_success_count,enchant_success_count "
                       "from user_equipment_state where u_id = ? order by slot_index asc");

    if (selectPreparedQuery(sql,
                            equipmentStateQuery,
                            sqlParams(boundPlayerId),
                            9,
                            equipmentStateRows)) {
        while (equipmentStateRows.size() >= 9) {
            int slotIndex = 0;
            try {
                slotIndex = std::stoi(trim(equipmentStateRows.front()));
            } catch (...) {
                slotIndex = -1;
            }
            equipmentStateRows.pop_front();

            auto readValue = [&equipmentStateRows, &trim]() -> int {
                int value = 0;
                try {
                    value = std::stoi(trim(equipmentStateRows.front()));
                } catch (...) {
                    value = 0;
                }
                equipmentStateRows.pop_front();
                return value;
            };

            const int itemId = readValue();
            const int enhanceLevel = readValue();
            const int forgeLevel = readValue();
            const int enchantKind = readValue();
            const int enchantValue = readValue();
            int enhanceSuccessCount = readValue();
            int forgeSuccessCount = readValue();
            int enchantSuccessCount = readValue();

            if (slotIndex < 0 || slotIndex >= MAX_EQUIPMENT_SLOT_NUM) {
                continue;
            }

            enhanceSuccessCount = qMax(enhanceSuccessCount, enhanceLevel);
            forgeSuccessCount = qMax(forgeSuccessCount, forgeLevel);
            if (enchantKind != 0) {
                enchantSuccessCount = qMax(enchantSuccessCount, 1);
            }

            initbag_rs.equippedItemIds[slotIndex] = itemId;
            initbag_rs.equippedEnhanceLevels[slotIndex] = enhanceLevel;
            initbag_rs.equippedForgeLevels[slotIndex] = forgeLevel;
            initbag_rs.equippedEnchantKinds[slotIndex] = enchantKind;
            initbag_rs.equippedEnchantValues[slotIndex] = enchantValue;
            initbag_rs.equippedEnhanceSuccessCounts[slotIndex] = enhanceSuccessCount;
            initbag_rs.equippedForgeSuccessCounts[slotIndex] = forgeSuccessCount;
            initbag_rs.equippedEnchantSuccessCounts[slotIndex] = enchantSuccessCount;
        }
    }

    initbag_rs.warehouseUnlockTier = 1;
    std::list<std::string> warehouseTierRows;
    const QString warehouseTierQuery =
        QStringLiteral("select warehouse_unlock_tier from user_progress_state where u_id = ? limit 1");
    if (selectPreparedQuery(sql,
                            warehouseTierQuery,
                            sqlParams(boundPlayerId),
                            1,
                            warehouseTierRows)
        && !warehouseTierRows.empty()) {
        try {
            initbag_rs.warehouseUnlockTier = qBound(1, std::stoi(trim(warehouseTierRows.front())), 4);
        } catch (...) {
            initbag_rs.warehouseUnlockTier = 1;
        }
    }

    std::list<std::string> warehouseRows;
    const QString warehouseQuery =
        QStringLiteral("select item_id,item_count from user_warehouse_item where u_id = ?");
    selectPreparedQuery(sql, warehouseQuery, sqlParams(boundPlayerId), 2, warehouseRows);

    // list -> vector，保证每行有两列
    auto it = liststr.begin();
    while(it != liststr.end())
    {
        std::vector<std::string> row;

        row.push_back(*it++);
        if(it == liststr.end()) break;

        row.push_back(*it++);
        if(row.size() == 2)
            result.push_back(row);
    }

    // 限制背包最大容量
    int itemCount = std::min((int)result.size(), MAX_BAG_ITEM_NUM);
    initbag_rs.itemAmount = itemCount;
    QVector<QPair<int, int>> authoritativeBagEntries;
    authoritativeBagEntries.reserve(itemCount);

    // 遍历填充背包
    bool allValid = true;  // 用于记录是否所有行都解析成功
    for(int i = 0; i < itemCount; ++i)
    {
        try {
            std::string idStr = trim(result[i][0]);
            std::string countStr = trim(result[i][1]);

            initbag_rs.playerBag[i][0] = std::stoi(idStr);
            initbag_rs.playerBag[i][1] = std::stoi(countStr);
            if (initbag_rs.playerBag[i][0] > 0 && initbag_rs.playerBag[i][1] > 0) {
                authoritativeBagEntries.push_back(qMakePair(initbag_rs.playerBag[i][0],
                                                            initbag_rs.playerBag[i][1]));
            }
        }
        catch(const std::invalid_argument& e) {
            qWarning() << "Invalid number in DB for player" << boundPlayerId
                       << "row" << i << ":"
                       << QString::fromStdString(result[i][0])
                       << QString::fromStdString(result[i][1]);
            initbag_rs.playerBag[i][0] = 0;
            initbag_rs.playerBag[i][1] = 0;
            allValid = false;
        }
        catch(const std::out_of_range& e) {
            qWarning() << "Number out of range in DB for player" << boundPlayerId
                       << "row" << i;
            initbag_rs.playerBag[i][0] = 0;
            initbag_rs.playerBag[i][1] = 0;
            allValid = false;
        }
    }

    std::vector<std::vector<std::string>> warehouseResult;
    auto warehouseIt = warehouseRows.begin();
    while (warehouseIt != warehouseRows.end())
    {
        std::vector<std::string> row;
        row.push_back(*warehouseIt++);
        if (warehouseIt == warehouseRows.end()) break;
        row.push_back(*warehouseIt++);
        if (row.size() == 2) {
            warehouseResult.push_back(row);
        }
    }

    const int warehouseItemCount = std::min((int)warehouseResult.size(), MAX_WAREHOUSE_ITEM_NUM);
    initbag_rs.warehouseItemAmount = warehouseItemCount;
    QVector<QPair<int, int>> authoritativeWarehouseEntries;
    authoritativeWarehouseEntries.reserve(warehouseItemCount);
    for (int i = 0; i < warehouseItemCount; ++i) {
        try {
            initbag_rs.warehouseItems[i][0] = std::stoi(trim(warehouseResult[i][0]));
            initbag_rs.warehouseItems[i][1] = std::stoi(trim(warehouseResult[i][1]));
            if (initbag_rs.warehouseItems[i][0] > 0 && initbag_rs.warehouseItems[i][1] > 0) {
                authoritativeWarehouseEntries.push_back(
                    qMakePair(initbag_rs.warehouseItems[i][0], initbag_rs.warehouseItems[i][1]));
            }
        } catch (...) {
            initbag_rs.warehouseItems[i][0] = 0;
            initbag_rs.warehouseItems[i][1] = 0;
            allValid = false;
        }
    }

    if (Player_Information* trackedPlayer = findTrackedPlayer(boundPlayerId)) {
        trackedPlayer->bagLoaded = true;
        trackedPlayer->authoritativeBagUpdatedAtMs = 0;
        trackedPlayer->warehouseEntries = authoritativeWarehouseEntries;
        trackedPlayer->warehouseLoaded = true;
        trackedPlayer->warehouseUnlockTier = initbag_rs.warehouseUnlockTier;
        for (int slotIndex = 0; slotIndex < MAX_EQUIPMENT_SLOT_NUM; ++slotIndex) {
            trackedPlayer->equippedItemIds[slotIndex] = initbag_rs.equippedItemIds[slotIndex];
            trackedPlayer->equippedEnhanceLevels[slotIndex] = initbag_rs.equippedEnhanceLevels[slotIndex];
            trackedPlayer->equippedForgeLevels[slotIndex] = initbag_rs.equippedForgeLevels[slotIndex];
            trackedPlayer->equippedEnchantKinds[slotIndex] = initbag_rs.equippedEnchantKinds[slotIndex];
            trackedPlayer->equippedEnchantValues[slotIndex] = initbag_rs.equippedEnchantValues[slotIndex];
            trackedPlayer->equippedEnhanceSuccessCounts[slotIndex] = initbag_rs.equippedEnhanceSuccessCounts[slotIndex];
            trackedPlayer->equippedForgeSuccessCounts[slotIndex] = initbag_rs.equippedForgeSuccessCounts[slotIndex];
            trackedPlayer->equippedEnchantSuccessCounts[slotIndex] = initbag_rs.equippedEnchantSuccessCounts[slotIndex];
        }
        EquipmentIntArray runtimeEquippedIds{};
        for (int slotIndex = 0; slotIndex < MAX_EQUIPMENT_SLOT_NUM; ++slotIndex) {
            runtimeEquippedIds[slotIndex] = trackedPlayer->equippedItemIds[slotIndex];
        }
        trackedPlayer->bagEntries = bagEntriesExcludingEquipped(authoritativeBagEntries, runtimeEquippedIds);
    }

    for (int i = 0;i < itemCount;i++) {
        qDebug()<<initbag_rs.playerBag[i][0]<<":"<<initbag_rs.playerBag[i][1];
    }
    // 设置最终结果状态
    initbag_rs.Result = allValid ? _initializebag_success : _initializebag_fail;
    qDebug()<<"Result:"<<initbag_rs.Result;
    // 发送给客户端
    QByteArray packet = PacketBuilder::build(_default_protocol_initbag_rs, initbag_rs);
    emit sendToClient(clientId, packet);
}

/**
 * @brief 处理Location_Request相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::Location_Request(quint64 clientId, STRU_LOCATION_RQ* rq)
{
    JaegerDebug();

    if (!rq) {
        return;
    }

    const int boundPlayerId = boundPlayerIdForClient(clientId);
    if (boundPlayerId <= 0 || boundPlayerId != rq->player_UserId) {
        qWarning() << "Location update rejected because client binding mismatched:"
                   << clientId << rq->player_UserId << boundPlayerId;
        return;
    }

    const Player_Information* previousTrackedPlayer = findTrackedPlayer(boundPlayerId);
    const QString previousMapId = previousTrackedPlayer ? previousTrackedPlayer->mapId : QString();
    const qint64 previousInstanceId = previousTrackedPlayer ? previousTrackedPlayer->instanceId : 0;

    QString requestedMapId = QString::fromUtf8(rq->mapId).trimmed();
    if (requestedMapId.isEmpty()) {
        requestedMapId = QStringLiteral("BornWorld");
    }
    const qint64 requestedInstanceId = qMax<qint64>(0, rq->instanceId);
    const qint64 authoritativeInstanceId =
        resolveAuthoritativeInstanceIdForPlayerMap(boundPlayerId, requestedMapId, requestedInstanceId);
    const bool changedScopedRoom =
        previousMapId.compare(requestedMapId, Qt::CaseInsensitive) != 0
        || previousInstanceId != authoritativeInstanceId;

    if (changedScopedRoom && isPartyDungeonMap(requestedMapId)) {
        resetDungeonRoomIfNoActivePlayers(requestedMapId, authoritativeInstanceId);
    }

    upsertTrackedPlayer(clientId,
                        boundPlayerId,
                        QString::fromUtf8(rq->player_Name),
                        requestedMapId,
                        authoritativeInstanceId,
                        rq->x,
                        rq->y,
                        rq->dir);

    if (changedScopedRoom && !previousMapId.trimmed().isEmpty()) {
        broadcastScopedLocationSnapshot(previousMapId, previousInstanceId);
        resetDungeonRoomIfNoActivePlayers(previousMapId, previousInstanceId);
    }

    STRU_LOCATION_RS sls{};
    int trackedCount = 0;
    for (const auto& trackedPlayer : players) {
        if (trackedCount >= 50) {
            break;
        }
        if (!isClientOnline(trackedPlayer.clientId)) {
            continue;
        }
        if (trackedPlayer.mapId.compare(requestedMapId, Qt::CaseInsensitive) != 0
            || trackedPlayer.instanceId != authoritativeInstanceId)
        {
            continue;
        }

        sls.players[trackedCount].player_UserId = trackedPlayer.player_UserId;
        std::strncpy(sls.players[trackedCount].player_Name,
                     trackedPlayer.playerName.toUtf8().constData(),
                     sizeof(sls.players[trackedCount].player_Name) - 1);
        sls.players[trackedCount].x = trackedPlayer.positionX;
        sls.players[trackedCount].y = trackedPlayer.positionY;
        sls.players[trackedCount].dir = trackedPlayer.direction;
        std::strncpy(sls.players[trackedCount].mapId,
                     trackedPlayer.mapId.toUtf8().constData(),
                     sizeof(sls.players[trackedCount].mapId) - 1);
        sls.players[trackedCount].instanceId = trackedPlayer.instanceId;
        ++trackedCount;
    }
    sls.playerCount = trackedCount;

    const QByteArray packet_loc = PacketBuilder::build(_default_protocol_location_rs, sls);
    const QVector<quint64> scopedClients = collectScopedClientIds(requestedMapId, authoritativeInstanceId);
    for (quint64 cid : scopedClients) {
        if (m_pKcpNet && m_pKcpNet->isKcpConnected(cid)) {
            emit sendToClientKcp(cid, packet_loc);
        } else {
            emit sendToClient(cid, packet_loc);
        }
    }

    syncDungeonRoomToClient(clientId, requestedMapId, authoritativeInstanceId);

    const PartyRuntimeState* party = findPartyByPlayerId(boundPlayerId);
    if (changedScopedRoom
        && isPartyDungeonMap(requestedMapId)
        && party
        && party->leaderPlayerId == boundPlayerId
        && party->memberPlayerIds.size() > 1)
    {
        broadcastPartyDungeonEntry(boundPlayerId,
                                   requestedMapId,
                                   authoritativeInstanceId,
                                   rq->x,
                                   rq->y);
    }
}

/**
 * @brief 处理HandlePartyAction相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::HandlePartyAction(quint64 clientId, STRU_PARTY_ACTION_RQ* rq)
{
    if (!rq) {
        return;
    }

    const int boundPlayerId = boundPlayerIdForClient(clientId);
    Player_Information* requester = findBoundTrackedPlayer(clientId, rq->requesterPlayerId);
    if (boundPlayerId <= 0 || !requester || requester->player_UserId != boundPlayerId) {
        qWarning() << "Party action rejected because client binding mismatched:"
                   << clientId << rq->requesterPlayerId << boundPlayerId;
        return;
    }

    const int requesterId = requester->player_UserId;
    const quint16 action = rq->action;
    const QString requesterName = !QString::fromUtf8(rq->requesterName).trimmed().isEmpty()
                                      ? QString::fromUtf8(rq->requesterName).trimmed()
                                      : requester->playerName;
    if (!requesterName.isEmpty()) {
        requester->playerName = requesterName;
    }

    auto sendError = [&](const QString& message) {
        sendPartyStateToPlayer(requesterId, action, 1, message, true);
    };

    auto createPartyForRequester = [&]() -> PartyRuntimeState* {
        PartyRuntimeState party;
        party.partyId = m_nextPartyId++;
        party.leaderPlayerId = requesterId;
        party.sharedInstanceId = m_nextPartySharedInstanceId++;
        party.memberPlayerIds.push_back(requesterId);
        m_partyStates.insert(party.partyId, party);
        m_playerPartyIds.insert(requesterId, party.partyId);
        m_pendingPartyInvites.remove(requesterId);
        return findPartyById(party.partyId);
    };

    switch (action) {
    case _party_action_create:
    {
        if (findPartyByPlayerId(requesterId)) {
            sendError(QStringLiteral("你已经在队伍中。"));
            return;
        }
        createPartyForRequester();
        sendPartyStateToPlayer(requesterId, action, 0, QStringLiteral("队伍已创建。"), true);
        return;
    }
    case _party_action_invite:
    {
        bool autoCreatedParty = false;
        PartyRuntimeState* party = findPartyByPlayerId(requesterId);
        if (!party) {
            party = createPartyForRequester();
            autoCreatedParty = true;
        }
        if (!party) {
            sendError(QStringLiteral("创建队伍失败，请稍后重试。"));
            return;
        }
        if (party->leaderPlayerId != requesterId) {
            sendError(QStringLiteral("只有队长才能邀请成员。"));
            return;
        }
        if (party->memberPlayerIds.size() >= MAX_PARTY_MEMBER_NUM) {
            sendError(QStringLiteral("队伍已满，无法继续邀请。"));
            return;
        }

        Player_Information* target = nullptr;
        if (rq->targetPlayerId > 0) {
            target = findTrackedPlayer(rq->targetPlayerId);
            if (target && !isClientOnline(target->clientId)) {
                target = nullptr;
            }
        }
        if (!target) {
            target = findTrackedPlayerByName(QString::fromUtf8(rq->targetName));
        }
        if (!target) {
            sendError(QStringLiteral("没有找到在线角色：%1").arg(QString::fromUtf8(rq->targetName).trimmed()));
            return;
        }
        if (target->player_UserId == requesterId) {
            sendError(QStringLiteral("不能邀请自己加入队伍。"));
            return;
        }
        if (findPartyByPlayerId(target->player_UserId)) {
            sendError(QStringLiteral("%1 已经在其他队伍中。").arg(target->playerName));
            return;
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        auto pendingInviteIt = m_pendingPartyInvites.find(target->player_UserId);
        if (pendingInviteIt != m_pendingPartyInvites.end()) {
            if (pendingInviteIt.value().expiresAtMs > 0 && pendingInviteIt.value().expiresAtMs <= nowMs) {
                pendingInviteIt = m_pendingPartyInvites.erase(pendingInviteIt);
            } else {
                sendError(QStringLiteral("%1 当前已有未处理的队伍邀请。").arg(target->playerName));
                return;
            }
        }

        PendingPartyInviteState invite;
        invite.inviterPlayerId = requesterId;
        invite.inviterName = requester->playerName;
        invite.partyId = party->partyId;
        invite.expiresAtMs = nowMs + 60000;
        m_pendingPartyInvites.insert(target->player_UserId, invite);

        const QString requesterMessage = autoCreatedParty
                                             ? QStringLiteral("已自动创建队伍，并向 %1 发出邀请。").arg(target->playerName)
                                             : QStringLiteral("已向 %1 发出队伍邀请。").arg(target->playerName);
        sendPartyStateToPlayer(requesterId, action, 0, requesterMessage, true);
        sendPartyStateToPlayer(target->player_UserId,
                               action,
                               0,
                               QStringLiteral("%1 邀请你加入队伍。").arg(requester->playerName),
                               true);
        return;
    }
    case _party_action_accept:
    {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        auto inviteIt = m_pendingPartyInvites.find(requesterId);
        if (inviteIt == m_pendingPartyInvites.end()) {
            sendError(QStringLiteral("当前没有待接受的队伍邀请。"));
            return;
        }
        const PendingPartyInviteState invite = inviteIt.value();
        if (invite.expiresAtMs > 0 && invite.expiresAtMs <= nowMs) {
            m_pendingPartyInvites.erase(inviteIt);
            sendError(QStringLiteral("这条队伍邀请已经过期。"));
            return;
        }
        if (findPartyByPlayerId(requesterId)) {
            m_pendingPartyInvites.erase(inviteIt);
            sendError(QStringLiteral("你已经在队伍中。"));
            return;
        }

        PartyRuntimeState* party = findPartyById(invite.partyId);
        if (!party) {
            m_pendingPartyInvites.erase(inviteIt);
            sendError(QStringLiteral("该队伍已经不存在了。"));
            return;
        }
        if (party->memberPlayerIds.size() >= MAX_PARTY_MEMBER_NUM) {
            m_pendingPartyInvites.erase(inviteIt);
            sendError(QStringLiteral("队伍已满，无法加入。"));
            return;
        }

        const int joinedPartyId = party->partyId;
        const int leaderPlayerId = party->leaderPlayerId;
        party->memberPlayerIds.push_back(requesterId);
        m_playerPartyIds.insert(requesterId, joinedPartyId);
        m_pendingPartyInvites.erase(inviteIt);

        bool syncDungeonEntry = false;
        QString leaderMapId;
        qint64 leaderInstanceId = 0;
        float leaderPosX = 0.0f;
        float leaderPosY = 0.0f;
        if (const Player_Information* leaderInfo = findTrackedPlayer(leaderPlayerId)) {
            if (isPartyDungeonMap(leaderInfo->mapId)) {
                syncDungeonEntry = true;
                leaderMapId = leaderInfo->mapId;
                leaderInstanceId = leaderInfo->instanceId;
                leaderPosX = leaderInfo->positionX;
                leaderPosY = leaderInfo->positionY;
            }
        }

        broadcastPartyState(joinedPartyId,
                            action,
                            0,
                            QStringLiteral("%1 加入了队伍。").arg(requester->playerName));
        if (syncDungeonEntry) {
            if (const PartyRuntimeState* refreshedParty = findPartyById(joinedPartyId)) {
                sendPartyDungeonEntryToPlayer(requesterId,
                                              *refreshedParty,
                                              leaderMapId,
                                              leaderInstanceId,
                                              leaderPosX,
                                              leaderPosY,
                                              QStringLiteral("队伍当前正在副本中，正在为你同步入场。"));
            }
        }
        return;
    }
    case _party_action_decline:
    {
        auto inviteIt = m_pendingPartyInvites.find(requesterId);
        if (inviteIt == m_pendingPartyInvites.end()) {
            sendError(QStringLiteral("当前没有待拒绝的队伍邀请。"));
            return;
        }

        const int inviterPlayerId = inviteIt.value().inviterPlayerId;
        m_pendingPartyInvites.erase(inviteIt);
        sendPartyStateToPlayer(requesterId, action, 0, QStringLiteral("你已拒绝队伍邀请。"), true);
        sendPartyStateToPlayer(inviterPlayerId,
                               action,
                               0,
                               QStringLiteral("%1 拒绝了你的队伍邀请。").arg(requester->playerName),
                               true);
        return;
    }
    case _party_action_leave:
    {
        const int partyId = m_playerPartyIds.value(requesterId, 0);
        PartyRuntimeState* party = findPartyById(partyId);
        if (!party) {
            sendError(QStringLiteral("你当前没有队伍。"));
            return;
        }

        party->memberPlayerIds.removeAll(requesterId);
        m_playerPartyIds.remove(requesterId);
        m_pendingPartyInvites.remove(requesterId);
        sendPartyStateToPlayer(requesterId, action, 0, QStringLiteral("你已离开队伍。"), true);

        if (party->memberPlayerIds.isEmpty()) {
            clearPendingInvitesForParty(partyId);
            m_partyStates.remove(partyId);
            return;
        }

        QString message = QStringLiteral("%1 离开了队伍。").arg(requester->playerName);
        if (party->leaderPlayerId == requesterId) {
            party->leaderPlayerId = party->memberPlayerIds.front();
            const Player_Information* newLeader = findTrackedPlayer(party->leaderPlayerId);
            const QString newLeaderName = newLeader ? newLeader->playerName : QStringLiteral("新队长");
            message += QStringLiteral(" %1 成为了新的队长。").arg(newLeaderName);
        }

        broadcastPartyState(partyId, action, 0, message);
        return;
    }
    case _party_action_kick:
    {
        PartyRuntimeState* party = findPartyByPlayerId(requesterId);
        if (!party) {
            sendError(QStringLiteral("你当前没有队伍。"));
            return;
        }
        if (party->leaderPlayerId != requesterId) {
            sendError(QStringLiteral("只有队长才能移出成员。"));
            return;
        }

        Player_Information* target = nullptr;
        if (rq->targetPlayerId > 0) {
            target = findTrackedPlayer(rq->targetPlayerId);
        }
        if (!target) {
            const QString targetName = QString::fromUtf8(rq->targetName).trimmed();
            for (int memberPlayerId : party->memberPlayerIds) {
                Player_Information* member = findTrackedPlayer(memberPlayerId);
                if (member && member->playerName.compare(targetName, Qt::CaseInsensitive) == 0) {
                    target = member;
                    break;
                }
            }
        }
        if (!target || !party->memberPlayerIds.contains(target->player_UserId)) {
            sendError(QStringLiteral("目标不在你的队伍中。"));
            return;
        }
        if (target->player_UserId == requesterId) {
            sendError(QStringLiteral("不能通过 kick 移出自己，请使用 /team leave。"));
            return;
        }

        party->memberPlayerIds.removeAll(target->player_UserId);
        m_playerPartyIds.remove(target->player_UserId);
        m_pendingPartyInvites.remove(target->player_UserId);
        sendPartyStateToPlayer(target->player_UserId, action, 0, QStringLiteral("你已被移出队伍。"), true);
        broadcastPartyState(party->partyId,
                            action,
                            0,
                            QStringLiteral("%1 被移出了队伍。").arg(target->playerName),
                            target->player_UserId);
        return;
    }
    case _party_action_query:
    {
        const QString message = findPartyByPlayerId(requesterId)
                                    ? QStringLiteral("队伍信息已同步。")
                                    : QStringLiteral("当前没有队伍。");
        sendPartyStateToPlayer(requesterId, action, 0, message, true);
        return;
    }
    default:
        sendError(QStringLiteral("未知的队伍操作。"));
        return;
    }
}
/**
 * @brief 处理PlayerList_Request相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::PlayerList_Request(quint64 clientId, STRU_PLAYERLIST_RQ* rq){
    Q_UNUSED(clientId);
    Q_UNUSED(rq);
}

/**
 * @brief 保存Save_Request相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::Save_Request(quint64 clientId, STRU_SAVE_RQ* rq){
    JaegerDebug();

    if (!rq) {
        return;
    }

    STRU_SAVE_RS sss;
    CMySql sql;
    sss.Save_Result = _save_fail_;
    const int boundPlayerId = boundPlayerIdForClient(clientId);
    if (boundPlayerId <= 0 || boundPlayerId != rq->player_UserId) {
        qWarning() << "Save rejected because client binding mismatched:"
                   << clientId << rq->player_UserId << boundPlayerId;
        const QByteArray packet = PacketBuilder::build(_default_protocol_save_rs, sss);
        emit sendToClient(clientId, packet);
        return;
    }
    QString normalizedMapId = QString::fromUtf8(rq->mapId).trimmed();
    if (normalizedMapId.isEmpty()) {
        normalizedMapId = QStringLiteral("BornWorld");
    }
    const qint64 normalizedInstanceId =
        resolveAuthoritativeInstanceIdForPlayerMap(boundPlayerId, normalizedMapId, rq->instanceId);
    if (!findTrackedPlayer(boundPlayerId)) {
        upsertTrackedPlayer(clientId,
                            boundPlayerId,
                            QString(),
                            normalizedMapId,
                            normalizedInstanceId,
                            rq->x,
                            rq->y,
                            3);
    }
    Player_Information* trackedPlayer = findTrackedPlayer(boundPlayerId);
    if (trackedPlayer) {
        trackedPlayer->clientId = clientId;
        trackedPlayer->mapId = normalizedMapId;
        trackedPlayer->instanceId = normalizedInstanceId;
        trackedPlayer->positionX = rq->x;
        trackedPlayer->positionY = rq->y;
    }

    if (!sql.BeginTransaction()) {
        const QByteArray packet = PacketBuilder::build(_default_protocol_save_rs, sss);
        emit sendToClient(clientId, packet);
        return;
    }
    const int persistedWarehouseUnlockTier =
        trackedPlayer ? qMax(1, trackedPlayer->warehouseUnlockTier) : 1;
    bool savedOk = upsertPlayerProgressState(sql,
                                             boundPlayerId,
                                             normalizedMapId,
                                             rq->questStep,
                                             persistedWarehouseUnlockTier,
                                             rq->x,
                                             rq->y);

    if (savedOk) {
        savedOk = sql.CommitTransaction();
        if (!savedOk) {
            sql.RollbackTransaction();
        }
    } else {
        sql.RollbackTransaction();
    }

    if(savedOk){
        sss.Save_Result = _save_success_;
    }else{
        sss.Save_Result = _save_error_;
    }
    QByteArray packet = PacketBuilder::build(_default_protocol_save_rs, sss);
    emit sendToClient(clientId, packet);
}

/**
 * @brief 处理HandleInventoryAction相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::HandleInventoryAction(quint64 clientId, STRU_INVENTORY_ACTION_RQ* rq)
{
    JaegerDebug();

    if (!rq) {
        return;
    }

    auto sendFailureWithoutSnapshot = [&](int playerId, quint16 action, int result, const QString& message) {
        STRU_INVENTORY_ACTION_RS rs{};
        rs.playerId = playerId;
        rs.action = action;
        rs.actionArg = rq->actionArg;
        rs.result = result;
        rs.requestItemId = rq->itemId;
        rs.requestItemCount = rq->itemCount;
        rs.requestSlotIndex = rq->slotIndex;
        std::strncpy(rs.message, message.toUtf8().constData(), sizeof(rs.message) - 1);
        const QByteArray packet = PacketBuilder::build(_default_protocol_inventory_action_rs, rs);
        emit sendToClient(clientId, packet);
    };

    const int boundPlayerId = boundPlayerIdForClient(clientId);
    Player_Information* trackedPlayer = findBoundTrackedPlayer(clientId, rq->playerId);
    if (boundPlayerId <= 0 || !trackedPlayer || trackedPlayer->player_UserId != boundPlayerId) {
        qWarning() << "Inventory action rejected because client binding mismatched:"
                   << clientId << rq->playerId << boundPlayerId;
        sendFailureWithoutSnapshot(boundPlayerId > 0 ? boundPlayerId : rq->playerId,
                                   rq->action,
                                   _inventory_action_result_invalid_request,
                                   QStringLiteral("角色尚未初始化，暂时无法处理该物品请求。"));
        return;
    }

    auto sendActionResponse = [&](const Player_Information& beforeInfo,
                                  const Player_Information& afterInfo,
                                  int result,
                                  const QString& message,
                                  quint16 responseFlags) {
        STRU_INVENTORY_ACTION_RS rs{};
        fillInventoryActionDeltaResponse(rs,
                                         beforeInfo,
                                         afterInfo,
                                         *rq,
                                         result,
                                         message,
                                         responseFlags);
        const QByteArray packet = PacketBuilder::build(_default_protocol_inventory_action_rs, rs);
        emit sendToClient(clientId, packet);
        return rs;
    };

    if (!trackedPlayer->bagLoaded || !trackedPlayer->warehouseLoaded) {
        sendActionResponse(*trackedPlayer,
                           *trackedPlayer,
                           _inventory_action_result_not_ready,
                           QStringLiteral("背包数据还没初始化完成，请稍后再试。"),
                           0);
        return;
    }

    const Player_Information previous = *trackedPlayer;
    Player_Information updated = *trackedPlayer;
    updated.clientId = clientId;
    updated.bagEntries = normalizedPersistEntries(updated.bagEntries);
    updated.warehouseEntries = normalizedPersistEntries(updated.warehouseEntries);
    updated.bagLoaded = true;
    updated.warehouseLoaded = true;

    int resultCode = _inventory_action_result_invalid_request;
    QString actionMessage = QStringLiteral("无效的物品操作请求。");
    bool shouldPersist = false;
    quint16 responseFlags = 0;

    switch (rq->action) {
    case _inventory_action_use_consumable:
    {
        const int itemId = qMax(0, rq->itemId);
        const ServerItemMetadata* metadata = serverItemMetadata(itemId);
        if (!metadata || metadata->kind != ServerItemKind::Consumable) {
            resultCode = _inventory_action_result_invalid_target;
            actionMessage = QStringLiteral("目标物品不是可使用的消耗品。");
            break;
        }
        if (trackedBagItemCount(updated, itemId) <= 0) {
            resultCode = _inventory_action_result_not_found;
            actionMessage = QStringLiteral("背包中没有 %1。").arg(metadata->name);
            break;
        }

        const CombatBalance::PlayerStats baseline = CombatBalance::playerStats(CombatBalance::clampLevel(updated.level));
        const int maxHealth = baseline.maxHealth + serverEquipmentMaxHealthBonus(updated);
        const int maxMana = baseline.maxMana + serverEquipmentMaxManaBonus(updated);
        const int healHp = qMax(0, metadata->healHp);
        const int healMp = qMax(0, metadata->healMp);
        const bool needHp = healHp > 0 && updated.health < maxHealth;
        const bool needMp = healMp > 0 && updated.mana < maxMana;
        if (!needHp && !needMp) {
            resultCode = _inventory_action_result_invalid_target;
            actionMessage = QStringLiteral("%1 当前无法发挥效果。").arg(metadata->name);
            break;
        }

        QString consumeFailure;
        if (!consumeTrackedBagCosts(updated, {{itemId, 1}}, &consumeFailure)) {
            resultCode = _inventory_action_result_not_found;
            actionMessage = consumeFailure.isEmpty() ? QStringLiteral("背包中缺少该消耗品。") : consumeFailure;
            break;
        }

        updated.health = qMin(maxHealth, updated.health + healHp);
        updated.mana = qMin(maxMana, updated.mana + healMp);
        clampTrackedVitalsToEquipmentCaps(updated);

        resultCode = _inventory_action_result_success;
        if (healHp > 0 && healMp > 0) {
            actionMessage = QStringLiteral("使用 %1 成功，恢复 HP %2、MP %3。")
                                .arg(metadata->name)
                                .arg(qMax(0, qMin(maxHealth, trackedPlayer->health + healHp) - trackedPlayer->health))
                                .arg(qMax(0, qMin(maxMana, trackedPlayer->mana + healMp) - trackedPlayer->mana));
        } else if (healHp > 0) {
            actionMessage = QStringLiteral("使用 %1 成功，恢复 HP %2。")
                                .arg(metadata->name)
                                .arg(qMax(0, qMin(maxHealth, trackedPlayer->health + healHp) - trackedPlayer->health));
        } else {
            actionMessage = QStringLiteral("使用 %1 成功，恢复 MP %2。")
                                .arg(metadata->name)
                                .arg(qMax(0, qMin(maxMana, trackedPlayer->mana + healMp) - trackedPlayer->mana));
        }
        shouldPersist = true;
        break;
    }
    case _inventory_action_equip_from_bag:
    {
        const int slotIndex = qBound(0, static_cast<int>(rq->actionArg), MAX_EQUIPMENT_SLOT_NUM - 1);
        QString failureMessage;
        if (!moveTrackedBagItemToEquipment(updated, qMax(0, rq->itemId), slotIndex, &failureMessage)) {
            resultCode = _inventory_action_result_invalid_target;
            actionMessage = failureMessage.isEmpty()
                                ? QStringLiteral("当前无法完成穿戴操作。")
                                : failureMessage;
            break;
        }

        clampTrackedVitalsToEquipmentCaps(updated);
        resultCode = _inventory_action_result_success;
        actionMessage = QStringLiteral("穿戴请求已由服务器确认。");
        shouldPersist = true;
        responseFlags |= _inventory_action_response_apply_local_layout;
        break;
    }
    case _inventory_action_unequip_to_bag:
    {
        const int slotIndex = qBound(0, rq->slotIndex, MAX_EQUIPMENT_SLOT_NUM - 1);
        QString failureMessage;
        if (!moveTrackedEquipmentToBag(updated, slotIndex, &failureMessage)) {
            resultCode = _inventory_action_result_invalid_target;
            actionMessage = failureMessage.isEmpty()
                                ? QStringLiteral("当前无法完成卸装操作。")
                                : failureMessage;
            break;
        }

        clampTrackedVitalsToEquipmentCaps(updated);
        resultCode = _inventory_action_result_success;
        actionMessage = QStringLiteral("卸装请求已由服务器确认。");
        shouldPersist = true;
        responseFlags |= _inventory_action_response_apply_local_layout;
        break;
    }
    case _inventory_action_move_equipment:
    {
        const int fromSlot = qBound(0, rq->slotIndex, MAX_EQUIPMENT_SLOT_NUM - 1);
        const int toSlot = qBound(0, static_cast<int>(rq->actionArg), MAX_EQUIPMENT_SLOT_NUM - 1);
        QString failureMessage;
        if (!moveTrackedEquipmentBetweenSlots(updated, fromSlot, toSlot, &failureMessage)) {
            resultCode = _inventory_action_result_invalid_target;
            actionMessage = failureMessage.isEmpty()
                                ? QStringLiteral("当前无法完成装备换位操作。")
                                : failureMessage;
            break;
        }

        resultCode = _inventory_action_result_success;
        actionMessage = QStringLiteral("装备换位已由服务器确认。");
        shouldPersist = true;
        responseFlags |= _inventory_action_response_apply_local_layout;
        break;
    }
    case _inventory_action_move_bag_to_warehouse:
    {
        const int transferCount = qMax(0, rq->itemCount);
        QString failureMessage;
        if (!moveTrackedBagItemsToWarehouse(updated, qMax(0, rq->itemId), transferCount, &failureMessage)) {
            resultCode = _inventory_action_result_invalid_target;
            actionMessage = failureMessage.isEmpty()
                                ? QStringLiteral("当前无法完成物品入库操作。")
                                : failureMessage;
            break;
        }

        resultCode = _inventory_action_result_success;
        actionMessage = transferCount > 0
                            ? QStringLiteral("物品已存入仓库。")
                            : QStringLiteral("仓库内同类物品布局已确认。");
        shouldPersist = transferCount > 0;
        responseFlags |= _inventory_action_response_apply_local_layout;
        break;
    }
    case _inventory_action_move_warehouse_to_bag:
    {
        const int transferCount = qMax(0, rq->itemCount);
        QString failureMessage;
        if (!moveTrackedWarehouseItemsToBag(updated, qMax(0, rq->itemId), transferCount, &failureMessage)) {
            resultCode = _inventory_action_result_invalid_target;
            actionMessage = failureMessage.isEmpty()
                                ? QStringLiteral("当前无法从仓库取回该物品。")
                                : failureMessage;
            break;
        }

        resultCode = _inventory_action_result_success;
        actionMessage = transferCount > 0
                            ? QStringLiteral("物品已从仓库取回。")
                            : QStringLiteral("背包内同类物品布局已确认。");
        shouldPersist = transferCount > 0;
        responseFlags |= _inventory_action_response_apply_local_layout;
        break;
    }
    case _inventory_action_unlock_warehouse:
    {
        const int nextTier = updated.warehouseUnlockTier + 1;
        if (nextTier > serverWarehouseTierCount()) {
            resultCode = _inventory_action_result_invalid_target;
            actionMessage = QStringLiteral("仓库已经全部解锁，不需要继续扩容。");
            break;
        }

        QString consumeFailure;
        if (!consumeTrackedBagCosts(updated, serverWarehouseUnlockCosts(nextTier), &consumeFailure)) {
            resultCode = _inventory_action_result_material_insufficient;
            actionMessage = consumeFailure;
            break;
        }

        updated.warehouseUnlockTier = nextTier;
        resultCode = _inventory_action_result_success;
        actionMessage = QStringLiteral("仓库已成功扩容到第 %1 层。").arg(nextTier);
        shouldPersist = true;
        break;
    }
    case _inventory_action_strengthen:
    case _inventory_action_forge:
    case _inventory_action_enchant:
    {
        const int slotIndex = rq->slotIndex;
        if (slotIndex < 0 || slotIndex >= MAX_EQUIPMENT_SLOT_NUM) {
            resultCode = _inventory_action_result_invalid_target;
            actionMessage = QStringLiteral("目标装备槽位无效。");
            break;
        }

        const int equippedItemId = updated.equippedItemIds[slotIndex];
        if (equippedItemId <= 0 || (rq->itemId > 0 && rq->itemId != equippedItemId)) {
            resultCode = _inventory_action_result_not_found;
            actionMessage = QStringLiteral("该槽位当前没有可打造的装备。");
            break;
        }

        const ServerItemMetadata* metadata = serverItemMetadata(equippedItemId);
        const QString equipName = (metadata && !metadata->name.isEmpty())
                                      ? metadata->name
                                      : QStringLiteral("装备%1").arg(equippedItemId);

        QVector<QPair<int, int>> costs;
        int successRate = 0;
        if (rq->action == _inventory_action_strengthen) {
            if (updated.equippedEnhanceLevels[slotIndex] >= serverInventoryRules().strengthen.maxLevel) {
                resultCode = _inventory_action_result_invalid_target;
                actionMessage = QStringLiteral("%1 已达到强化上限 +%2。")
                                    .arg(equipName)
                                    .arg(serverInventoryRules().strengthen.maxLevel);
                break;
            }
            costs = serverStrengthenCosts(updated, slotIndex);
            successRate = serverStrengthenSuccessRate(updated, slotIndex);
        } else if (rq->action == _inventory_action_forge) {
            if (updated.equippedForgeLevels[slotIndex] >= serverInventoryRules().forge.maxLevel) {
                resultCode = _inventory_action_result_invalid_target;
                actionMessage = QStringLiteral("%1 已达到锻造上限 +%2。")
                                    .arg(equipName)
                                    .arg(serverInventoryRules().forge.maxLevel);
                break;
            }
            costs = serverForgeCosts(updated, slotIndex);
            successRate = serverForgeSuccessRate(updated, slotIndex);
        } else {
            const int enchantKind = qBound(0, static_cast<int>(rq->actionArg), 4);
            const bool isWeapon = (metadata && metadata->kind == ServerItemKind::Weapon)
                                  || slotIndex == kEquipmentWeaponSlotIndex;
            if (enchantKind == kEnchantKindNone) {
                resultCode = _inventory_action_result_invalid_target;
                actionMessage = QStringLiteral("请选择有效的附魔类型。");
                break;
            }
            if (isWeapon) {
                if (enchantKind != kEnchantKindAttack && enchantKind != kEnchantKindCritical)
                {
                    resultCode = _inventory_action_result_invalid_target;
                    actionMessage = QStringLiteral("武器只能选择攻击或暴击附魔。");
                    break;
                }
            } else {
                if (enchantKind != kEnchantKindDefence
                    && enchantKind != kEnchantKindVitality
                    && enchantKind != kEnchantKindCritical)
                {
                    resultCode = _inventory_action_result_invalid_target;
                    actionMessage = QStringLiteral("护甲只能选择防御、生命或暴击附魔。");
                    break;
                }
            }
            costs = serverEnchantCosts(updated, slotIndex);
            successRate = serverEnchantSuccessRate(updated, slotIndex);
        }

        QString consumeFailure;
        if (!consumeTrackedBagCosts(updated, costs, &consumeFailure)) {
            resultCode = _inventory_action_result_material_insufficient;
            actionMessage = consumeFailure;
            break;
        }

        const bool success = serverRollChance(successRate / 100.0);
        if (rq->action == _inventory_action_strengthen) {
            if (success) {
                updated.equippedEnhanceSuccessCounts[slotIndex] = normalizedTrackedEnhanceSuccessCount(updated, slotIndex) + 1;
                updated.equippedEnhanceLevels[slotIndex] = qMax(0, updated.equippedEnhanceLevels[slotIndex]) + 1;
                resultCode = _inventory_action_result_success;
                actionMessage = QStringLiteral("%1 强化成功，当前为 +%2。")
                                    .arg(equipName)
                                    .arg(updated.equippedEnhanceLevels[slotIndex]);
            } else {
                clearTrackedEquipmentSlot(updated, slotIndex);
                resultCode = _inventory_action_result_failed;
                actionMessage = QStringLiteral("%1 强化失败，装备已损毁。").arg(equipName);
            }
        } else if (rq->action == _inventory_action_forge) {
            if (success) {
                updated.equippedForgeSuccessCounts[slotIndex] = normalizedTrackedForgeSuccessCount(updated, slotIndex) + 1;
                updated.equippedForgeLevels[slotIndex] = qMax(0, updated.equippedForgeLevels[slotIndex]) + 1;
                resultCode = _inventory_action_result_success;
                actionMessage = QStringLiteral("%1 锻造成功，当前为 +%2。")
                                    .arg(equipName)
                                    .arg(updated.equippedForgeLevels[slotIndex]);
            } else {
                clearTrackedEquipmentSlot(updated, slotIndex);
                resultCode = _inventory_action_result_failed;
                actionMessage = QStringLiteral("%1 锻造失败，装备已损毁。").arg(equipName);
            }
        } else {
            const int enchantKind = qBound(0, static_cast<int>(rq->actionArg), 4);
            const InventoryEnchantValues& enchantValues = serverInventoryRules().enchantValues;
            if (success) {
                updated.equippedEnchantSuccessCounts[slotIndex] = normalizedTrackedEnchantSuccessCount(updated, slotIndex) + 1;
                updated.equippedEnchantKinds[slotIndex] = enchantKind;
                switch (enchantKind) {
                case kEnchantKindAttack:
                    updated.equippedEnchantValues[slotIndex] = enchantValues.attack;
                    break;
                case kEnchantKindDefence:
                    updated.equippedEnchantValues[slotIndex] = enchantValues.defence;
                    break;
                case kEnchantKindVitality:
                    updated.equippedEnchantValues[slotIndex] = enchantValues.vitality;
                    break;
                case kEnchantKindCritical:
                    updated.equippedEnchantValues[slotIndex] =
                        ((metadata && metadata->kind == ServerItemKind::Weapon)
                         || slotIndex == kEquipmentWeaponSlotIndex)
                            ? enchantValues.criticalWeapon
                            : enchantValues.criticalArmor;
                    break;
                case kEnchantKindNone:
                    updated.equippedEnchantValues[slotIndex] = 0;
                    break;
                }

                resultCode = _inventory_action_result_success;
                actionMessage = QStringLiteral("%1 附魔成功，词条已更新。").arg(equipName);
            } else {
                clearTrackedEquipmentSlot(updated, slotIndex);
                resultCode = _inventory_action_result_failed;
                actionMessage = QStringLiteral("%1 附魔失败，装备已损毁。").arg(equipName);
            }
        }

        clampTrackedVitalsToEquipmentCaps(updated);
        shouldPersist = true;
        break;
    }
    default:
        break;
    }

    if (!shouldPersist) {
        sendActionResponse(previous, updated, resultCode, actionMessage, responseFlags);
        return;
    }

    updated.inventoryStateVersion = trackedPlayer->inventoryStateVersion + 1;
    STRU_INVENTORY_ACTION_RS stagedResponse{};
    fillInventoryActionDeltaResponse(stagedResponse,
                                     previous,
                                     updated,
                                     *rq,
                                     resultCode,
                                     actionMessage,
                                     responseFlags);
    QString stageError;
    if (!stageInventoryPersistence(updated,
                                   inventoryActionName(rq->action),
                                   inventoryActionJournalPayload(stagedResponse),
                                   &stageError))
    {
        sendActionResponse(previous,
                           previous,
                           _inventory_action_result_server_error,
                           stageError.isEmpty()
                               ? QStringLiteral("库存状态暂时无法持久化，请稍后重试。")
                               : stageError,
                           0);
        return;
    }

    *trackedPlayer = updated;
    trackedPlayer->clientId = clientId;
    sendActionResponse(previous, *trackedPlayer, resultCode, actionMessage, responseFlags);
}

/**
 * @brief 处理Dazuo_Request相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::Dazuo_Request(quint64 clientId, STRU_DAZUO_RQ* rq)
{
    JaegerDebug();
    if (!rq) {
        return;
    }
    STRU_DAZUO_RS sds;
    const int boundPlayerId = boundPlayerIdForClient(clientId);
    if (boundPlayerId <= 0 || boundPlayerId != rq->player_UserId) {
        qWarning() << "Dazuo rejected because client binding mismatched:"
                   << clientId << rq->player_UserId << boundPlayerId;
        return;
    }
    int tempid = boundPlayerId;
    sds.dazuoConfirm = false;

    if (rq->isDazuo)
    {
        CMySql sql;
        std::list<std::string> liststr;
        const QString levelQuery =
            QStringLiteral("SELECT u_level, u_experience FROM user_basic_information WHERE u_id = ?;");
        selectPreparedQuery(sql, levelQuery, sqlParams(tempid), 2, liststr);

        if (liststr.size() >= 2)
        {
            std::string userLevel = liststr.front(); liststr.pop_front();
            std::string userExp = liststr.front(); liststr.pop_front();

            const int parsedLevel = CombatBalance::clampLevel(std::stoi(userLevel));
            const long long parsedExp = qMax(0LL, std::stoll(userExp));
            const CombatBalance::PlayerStats baseline = CombatBalance::playerStats(parsedLevel);
            auto playerInfo = std::make_shared<Player_Information>(
                tempid,
                "",
                parsedLevel,
                parsedExp,
                parsedExp,
                baseline.maxHealth,
                baseline.maxMana,
                baseline.attack,
                baseline.magicAttack,
                baseline.independentAttack,
                baseline.defence,
                baseline.magicDefence,
                baseline.strength,
                baseline.intelligence,
                baseline.vitality,
                baseline.spirit,
                baseline.critRate,
                baseline.magicCritRate,
                baseline.critDamage,
                baseline.attackSpeed,
                baseline.moveSpeed,
                baseline.castSpeed,
                baseline.attackRange,
                0.0f,
                0.0f);
            m_mapPlayerInfo[tempid] = playerInfo;

            if (m_mapDazuoTimer.count(tempid) && m_mapDazuoTimer[tempid])
            {
                m_mapDazuoTimer[tempid]->stop();
                delete m_mapDazuoTimer[tempid];
                m_mapDazuoTimer[tempid] = nullptr;
            }

            QTimer* timer = new QTimer(this);
            quint64 cid = clientId;
            connect(timer, &QTimer::timeout, this, [this, tempid, cid]() {
                auto it = m_mapPlayerInfo.find(tempid);
                if (it == m_mapPlayerInfo.end()) return;
                auto info = it->second;
                long long gainedExp = info->level * 2;
                info->exp += gainedExp;
                qDebug() << "玩家ID：" << tempid << ", 打坐中... 获得经验：" << gainedExp << " 当前总经验：" << info->exp;
                bool leveled = LevelUp(info->level, info->exp, tempid);
                if (leveled)
                {
                    qDebug() << "玩家升级, 当前等级：" << info->level;
                    STRU_LEVELUP_RS sls;
                    sls.MaxExp = getEXP(info->level);
                    sls.userlevel = info->level;
                    QByteArray pkt = PacketBuilder::build(_default_protocol_levelup_rs, sls);
                    emit sendToClient(cid, pkt);
                }
            });
            timer->start(2000);
            m_mapDazuoTimer[tempid] = timer;

            sds.exp = playerInfo->exp;
            sds.userlevel = playerInfo->level;
            sds.dazuoConfirm = true;
            sds.MaxExp = getEXP(playerInfo->level);
            QByteArray packet = PacketBuilder::build(_default_protocol_dazuo_rs, sds);
            emit sendToClient(clientId, packet);
        }
    }
    else
    {
        if (m_mapDazuoTimer.count(tempid) && m_mapDazuoTimer[tempid])
        {
            m_mapDazuoTimer[tempid]->stop();
            delete m_mapDazuoTimer[tempid];
            m_mapDazuoTimer.erase(tempid);
            qDebug() << "玩家ID：" << tempid << ", 打坐结束";
        }

        auto infoIt = m_mapPlayerInfo.find(tempid);
        if (infoIt != m_mapPlayerInfo.end())
        {
            auto info = infoIt->second;
            CMySql sql;
            updatePreparedQuery(sql,
                                QStringLiteral("UPDATE user_basic_information SET "
                                               "u_experience = ?, "
                                               "u_level = ? "
                                               "WHERE u_id = ?;"),
                                sqlParams(static_cast<qlonglong>(info->exp),
                                          info->level,
                                          tempid));

            sds.exp = info->exp;
            sds.userlevel = info->level;
            sds.dazuoConfirm = false;
            sds.addedExp = info->exp - info->startExp;
            qDebug() << "玩家ID:" << tempid << "打坐获得经验:" << sds.addedExp << "总经验:" << sds.exp;
            QByteArray packet = PacketBuilder::build(_default_protocol_dazuo_rs, sds);
            emit sendToClient(clientId, packet);

            m_mapPlayerInfo.erase(tempid);
        }
    }
}

/**
 * @brief 处理LevelUp相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool core::LevelUp(int& lvl,long long& userExp,int player_id)
{
    JaegerDebug();
    CMySql sql;
    bool leveledUp = false;
    int newLevel = CombatBalance::clampLevel(lvl);
    long long remainedExp = qMax(0LL, userExp);

    while(remainedExp >= getEXP(newLevel) && newLevel < 100){
        remainedExp -= getEXP(newLevel);
        ++newLevel;
        leveledUp = true;
    }

    lvl = newLevel;
    userExp = remainedExp;

    if(leveledUp){
        const CombatBalance::PlayerStats baseline = CombatBalance::playerStats(lvl);
        if (m_mapPlayerInfo.count(player_id) && m_mapPlayerInfo[player_id]) {
            m_mapPlayerInfo[player_id]->exp = userExp;
            m_mapPlayerInfo[player_id]->level = lvl;
            m_mapPlayerInfo[player_id]->health = baseline.maxHealth;
            m_mapPlayerInfo[player_id]->mana = baseline.maxMana;
            m_mapPlayerInfo[player_id]->attackPower = baseline.attack;
            m_mapPlayerInfo[player_id]->magicAttack = baseline.magicAttack;
            m_mapPlayerInfo[player_id]->independentAttack = baseline.independentAttack;
            m_mapPlayerInfo[player_id]->defense = baseline.defence;
            m_mapPlayerInfo[player_id]->magicDefense = baseline.magicDefence;
            m_mapPlayerInfo[player_id]->strength = baseline.strength;
            m_mapPlayerInfo[player_id]->intelligence = baseline.intelligence;
            m_mapPlayerInfo[player_id]->vitality = baseline.vitality;
            m_mapPlayerInfo[player_id]->spirit = baseline.spirit;
            m_mapPlayerInfo[player_id]->critRate = baseline.critRate;
            m_mapPlayerInfo[player_id]->magicCritRate = baseline.magicCritRate;
            m_mapPlayerInfo[player_id]->critDamage = baseline.critDamage;
            m_mapPlayerInfo[player_id]->attackSpeed = baseline.attackSpeed;
            m_mapPlayerInfo[player_id]->moveSpeed = baseline.moveSpeed;
            m_mapPlayerInfo[player_id]->castSpeed = baseline.castSpeed;
            m_mapPlayerInfo[player_id]->attackRange = baseline.attackRange;
        }
        return updateUserBasicInformation(sql,
                                          player_id,
                                          baseline.maxHealth,
                                          baseline.maxMana,
                                          baseline.attack,
                                          baseline.magicAttack,
                                          baseline.independentAttack,
                                          baseline.attackRange,
                                          userExp,
                                          lvl,
                                          baseline.defence,
                                          baseline.magicDefence,
                                          baseline.strength,
                                          baseline.intelligence,
                                          baseline.vitality,
                                          baseline.spirit,
                                          baseline.critRate,
                                          baseline.magicCritRate,
                                          baseline.critDamage,
                                          baseline.attackSpeed,
                                          baseline.moveSpeed,
                                          baseline.castSpeed);
    }
    return false;
}

/**
 * @brief 处理HandleAttack相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::HandleAttack(quint64 clientId, STRU_ATTACK_RQ* rq) {
    JaegerDebug();

    if (!rq) {
        return;
    }

    const Player_Information* attackerInfo = findBoundTrackedPlayer(clientId, rq->attackerId);
    const Player_Information* targetInfo = findTrackedPlayer(rq->targetId);
    if (!attackerInfo || !targetInfo) {
        qWarning() << "Attack rejected because tracked instance state is missing."
                   << "attacker:" << rq->attackerId
                   << "target:" << rq->targetId;
        return;
    }
    if (attackerInfo->mapId.compare(targetInfo->mapId, Qt::CaseInsensitive) != 0
        || attackerInfo->instanceId != targetInfo->instanceId)
    {
        qWarning() << "Attack rejected because players are not in the same scoped dungeon instance."
                   << "attacker:" << attackerInfo->mapId << attackerInfo->instanceId
                   << "target:" << targetInfo->mapId << targetInfo->instanceId;
        return;
    }

    // 获取目标血量
    int targetHealth = getHealth(rq->targetId);
    if (targetHealth <= 0) {
        qWarning() << "目标玩家" << rq->targetId << "已经死亡或不存在";
        return;
    }

    // 简化伤害计算：直接使用客户端传来的伤害值
    int finalDamage = std::max(1, (int)rq->damage);
    int newHealth = std::max(0, targetHealth - finalDamage);

    if (!updateHealthInDB(rq->targetId, newHealth)) {
        qWarning() << "更新目标玩家" << rq->targetId << "的血量失败";
        return;
    }

    STRU_ATTACK_RS rs;
    rs.targetId = rq->targetId;
    rs.currentHealth = newHealth;
    rs.isDead = (newHealth <= 0);

    QByteArray packet = PacketBuilder::build(_default_protocol_attack_rs, rs);
    const QVector<quint64> scopedClients = collectScopedClientIds(attackerInfo->mapId, attackerInfo->instanceId);
    for (quint64 cid : scopedClients) {
        if (m_pKcpNet && m_pKcpNet->isKcpConnected(cid)) {
            emit sendToClientKcp(cid, packet);
        } else {
            emit sendToClient(cid, packet);
        }
    }

    // 如果死亡，10秒后复活
    if (rs.isDead) {
        int deadId = rs.targetId;
        const QString deathMapId = attackerInfo->mapId;
        const qint64 deathInstanceId = attackerInfo->instanceId;
        QTimer::singleShot(10000, this, [this, deadId, deathMapId, deathInstanceId]() {
            int reviveHealth = 100;
            CMySql sql;
            std::list<std::string> levelResult;
            const QString levelQuery =
                QStringLiteral("SELECT u_level FROM user_basic_information WHERE u_id = ? LIMIT 1;");
            if (selectPreparedQuery(sql, levelQuery, sqlParams(deadId), 1, levelResult)
                && !levelResult.empty()) {
                try {
                    reviveHealth = static_cast<int>(getHP(std::stoi(levelResult.front())));
                } catch (...) {
                    reviveHealth = 100;
                }
            }

            updateHealthInDB(deadId, reviveHealth);
            STRU_REVIVE_RS srr;
            srr.playerId = deadId;
            srr.newhealth = reviveHealth;
            QByteArray revivePacket = PacketBuilder::build(_default_protocol_revive_rs, srr);
            const QVector<quint64> scopedClients = collectScopedClientIds(deathMapId, deathInstanceId);
            for (quint64 cid : scopedClients) {
                emit sendToClient(cid, revivePacket);
            }
        });
    }
}

/**
 * @brief 处理HandleMonsterHit相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::HandleMonsterHit(quint64 clientId, STRU_MONSTER_HIT_RQ* rq)
{
    if (!rq) {
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    pruneExpiredSkillHitWindows(nowMs);

    Player_Information* attackerInfo = findBoundTrackedPlayer(clientId, rq->attackerId);
    if (!attackerInfo) {
        qWarning() << "Monster hit rejected because attacker instance state is missing or mismatched:"
                   << clientId << rq->attackerId << boundPlayerIdForClient(clientId);
        return;
    }

    DungeonRoomState* room = ensureDungeonRoom(attackerInfo->mapId, attackerInfo->instanceId);
    if (!room || !room->monsters.contains(rq->monsterRuntimeId) || !room->spawns.contains(rq->monsterRuntimeId)) {
        return;
    }

    MonsterState& monster = room->monsters[rq->monsterRuntimeId];
    const MonsterSpawnDefinition spawn = room->spawns.value(rq->monsterRuntimeId);
    if (!monster.alive || monster.health <= 0) {
        return;
    }

    const QString skillId = normalizedSkillId(rq->skillId);
    const bool isSkillHit = (rq->hitFlags & _monster_hit_flag_skill) != 0;
    const bool isPersistentHit = (rq->hitFlags & _monster_hit_flag_persistent) != 0;
    const QRectF monsterBounds(monster.position.x(), monster.position.y(), kServerNpcSize, kServerNpcSize);

    QRectF hitRect;
    bool hasValidHitRect = false;
    if (isPersistentHit && !skillId.isEmpty()) {
        const QString key = buildSkillHitWindowKey(attackerInfo->player_UserId, skillId);
        const auto windowIt = m_skillHitWindows.constFind(key);
        if (windowIt != m_skillHitWindows.constEnd()
            && windowIt->persistent
            && windowIt->mapId.compare(attackerInfo->mapId, Qt::CaseInsensitive) == 0
            && windowIt->instanceId == attackerInfo->instanceId
            && windowIt->expiresAtMs > nowMs)
        {
            hitRect = windowIt->rect;
            hasValidHitRect = hitRect.isValid() && !hitRect.isNull();
        }
    } else if (isSkillHit && !skillId.isEmpty()) {
        hitRect = buildServerSkillImpactRect(*attackerInfo, skillId);
        hasValidHitRect = hitRect.isValid() && !hitRect.isNull();
        if (!hasValidHitRect) {
            const QString key = buildSkillHitWindowKey(attackerInfo->player_UserId, skillId);
            const auto windowIt = m_skillHitWindows.constFind(key);
            if (windowIt != m_skillHitWindows.constEnd()
                && windowIt->mapId.compare(attackerInfo->mapId, Qt::CaseInsensitive) == 0
                && windowIt->instanceId == attackerInfo->instanceId
                && windowIt->expiresAtMs > nowMs)
            {
                hitRect = windowIt->rect;
                hasValidHitRect = hitRect.isValid() && !hitRect.isNull();
            }
        }
    } else {
        hitRect = buildBasicAttackRect(*attackerInfo);
        hasValidHitRect = hitRect.isValid() && !hitRect.isNull();
    }

    if (!hasValidHitRect || !hitRect.intersects(monsterBounds)) {
        STRU_MONSTER_HIT_RS rejectedRs{};
        rejectedRs.attackerId = attackerInfo->player_UserId;
        rejectedRs.monsterRuntimeId = rq->monsterRuntimeId;
        rejectedRs.appliedDamage = 0;
        rejectedRs.currentHealth = monster.health;
        rejectedRs.maxHealth = monster.maxHealth;
        rejectedRs.isDead = !monster.alive;
        std::memcpy(rejectedRs.skillId, rq->skillId, sizeof(rejectedRs.skillId));
        const QByteArray rejectedPacket = PacketBuilder::build(_default_protocol_monster_hit_rs, rejectedRs);
        emit sendToClient(clientId, rejectedPacket);
        return;
    }

    const int resolvedMonsterLevel = resolveDungeonMonsterLevel(*room, spawn);
    const int appliedDamage = isSkillHit
                                  ? computeServerSkillDamage(*attackerInfo,
                                                             skillId,
                                                             rq->hitFlags,
                                                             resolvedMonsterLevel,
                                                             spawn.monsterTier)
                                  : computeServerBasicAttackDamage(*attackerInfo,
                                                                   resolvedMonsterLevel,
                                                                   spawn.monsterTier);
    ParticipationRecord& contribution = monster.contributorUpdatedAtMs[attackerInfo->player_UserId];
    contribution.lastUpdatedAtMs = nowMs;
    contribution.totalDamage += qMax(0, appliedDamage);
    contribution.hitCount += 1;
    noteDungeonParticipant(*room, attackerInfo->player_UserId, nowMs, appliedDamage);
    monster.chasing = true;
    monster.health = qMax(0, monster.health - appliedDamage);
    if (monster.health <= 0) {
        monster.alive = false;
        monster.chasing = false;
        monster.respawnAtMs = QDateTime::currentMSecsSinceEpoch() + qMax(0, spawn.respawnMs);
    }
    if (refreshDungeonRoomProgress(*room)) {
        room->dirty = true;
    }
    room->dirty = true;

    STRU_MONSTER_HIT_RS rs{};
    rs.attackerId = attackerInfo->player_UserId;
    rs.monsterRuntimeId = rq->monsterRuntimeId;
    rs.appliedDamage = appliedDamage;
    rs.currentHealth = monster.health;
    rs.maxHealth = monster.maxHealth;
    rs.isDead = !monster.alive;
    std::memcpy(rs.skillId, rq->skillId, sizeof(rs.skillId));
    const QByteArray packet = PacketBuilder::build(_default_protocol_monster_hit_rs, rs);
    emit sendToClient(clientId, packet);

    if (rs.isDead) {
        const RewardParticipationRule monsterRule = serverRewardRules().monsterReward;
        const int minimumContributionDamage =
            qMax(monsterRule.minimumDamage,
                 qRound(monster.maxHealth * monsterRule.minimumDamageRatio));
        const QVector<Player_Information*> rewardRecipients =
            collectEligibleRewardRecipients(*room,
                                            attackerInfo->player_UserId,
                                            monster.contributorUpdatedAtMs,
                                            nowMs,
                                            monsterRule.participationWindowMs,
                                            minimumContributionDamage,
                                            monsterRule.minimumHitCount);
        for (Player_Information* trackedPlayer : rewardRecipients) {
            if (!trackedPlayer) {
                continue;
            }

            const Player_Information previousRewardState = *trackedPlayer;
            Player_Information updatedRewardState = previousRewardState;
            bool leveledUp = false;
            const int expReward = applyServerMonsterRewardProgression(updatedRewardState,
                                                                      attackerInfo->mapId,
                                                                      resolvedMonsterLevel,
                                                                      spawn.monsterTier,
                                                                      &leveledUp);
            MonsterSpawnDefinition rewardDefinition = spawn;
            rewardDefinition.monsterLevel = resolvedMonsterLevel;
            const QVector<QPair<int, int>> plannedDrops = serverGenerateMonsterDrops(rewardDefinition);
            const QVector<QPair<int, int>> acceptedDrops = applyDropsToTrackedBag(updatedRewardState,
                                                                                  plannedDrops,
                                                                                  nowMs);
            updatedRewardState.inventoryStateVersion = previousRewardState.inventoryStateVersion + 1;

            QJsonObject rewardEvent;
            rewardEvent.insert(QStringLiteral("type"), QStringLiteral("monster_reward"));
            rewardEvent.insert(QStringLiteral("killerPlayerId"), attackerInfo->player_UserId);
            rewardEvent.insert(QStringLiteral("monsterRuntimeId"), rq->monsterRuntimeId);
            rewardEvent.insert(QStringLiteral("monsterName"), spawn.displayName);
            rewardEvent.insert(QStringLiteral("expReward"), expReward);
            rewardEvent.insert(QStringLiteral("mapId"), attackerInfo->mapId);
            rewardEvent.insert(QStringLiteral("monsterLevel"), resolvedMonsterLevel);
            rewardEvent.insert(QStringLiteral("leveledUp"), leveledUp);
            rewardEvent.insert(QStringLiteral("drops"),
                               inventoryJournalItemEntriesToJson(acceptedDrops));
            QString stageError;
            if (!stageInventoryPersistence(updatedRewardState,
                                           QStringLiteral("monster_reward"),
                                           QString::fromUtf8(
                                               QJsonDocument(rewardEvent).toJson(QJsonDocument::Compact)),
                                           &stageError))
            {
                qWarning() << "Failed to stage monster reward persistence:"
                           << trackedPlayer->player_UserId
                           << stageError;
                continue;
            }

            *trackedPlayer = updatedRewardState;

            STRU_MONSTER_REWARD_RS rewardRs{};
            rewardRs.recipientPlayerId = trackedPlayer->player_UserId;
            rewardRs.killerPlayerId = attackerInfo->player_UserId;
            rewardRs.monsterRuntimeId = rq->monsterRuntimeId;
            std::strncpy(rewardRs.monsterName,
                         spawn.displayName.toUtf8().constData(),
                         sizeof(rewardRs.monsterName) - 1);
            rewardRs.expReward = expReward;
            rewardRs.newExperience = trackedPlayer->exp;
            rewardRs.newLevel = trackedPlayer->level;
            rewardRs.health = trackedPlayer->health;
            rewardRs.mana = trackedPlayer->mana;
            rewardRs.attackPower = trackedPlayer->attackPower;
            rewardRs.magicAttack = trackedPlayer->magicAttack;
            rewardRs.independentAttack = trackedPlayer->independentAttack;
            rewardRs.defence = trackedPlayer->defense;
            rewardRs.magicDefence = trackedPlayer->magicDefense;
            rewardRs.strength = trackedPlayer->strength;
            rewardRs.intelligence = trackedPlayer->intelligence;
            rewardRs.vitality = trackedPlayer->vitality;
            rewardRs.spirit = trackedPlayer->spirit;
            rewardRs.criticalRate = trackedPlayer->critRate;
            rewardRs.magicCriticalRate = trackedPlayer->magicCritRate;
            rewardRs.criticalDamage = trackedPlayer->critDamage;
            rewardRs.attackSpeed = trackedPlayer->attackSpeed;
            rewardRs.moveSpeed = trackedPlayer->moveSpeed;
            rewardRs.castSpeed = trackedPlayer->castSpeed;
            rewardRs.attackRange = qRound(trackedPlayer->attackRange);
            rewardRs.rewardFlags = leveledUp ? _monster_reward_flag_level_up : 0;

            const int safeDropCount = qMin(acceptedDrops.size(), MAX_MONSTER_REWARD_ITEM_NUM);
            rewardRs.dropCount = safeDropCount;
            for (int i = 0; i < safeDropCount; ++i) {
                rewardRs.drops[i].itemId = acceptedDrops[i].first;
                rewardRs.drops[i].count = acceptedDrops[i].second;
            }

            const QByteArray rewardPacket = PacketBuilder::build(_default_protocol_monster_reward_rs, rewardRs);
            emit sendToClient(trackedPlayer->clientId, rewardPacket);
        }
    }

    broadcastDungeonRoomSnapshot(attackerInfo->mapId, attackerInfo->instanceId);
}

/**
 * @brief 同步HandleSkillStateSync相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::HandleSkillStateSync(quint64 clientId, STRU_SKILLSTATE_RQ* rq)
{
    if (!rq) {
        return;
    }

    Player_Information* trackedPlayer = findBoundTrackedPlayer(clientId, rq->playerId);
    if (!trackedPlayer) {
        qWarning() << "Skill state sync rejected because client binding mismatched:"
                   << clientId << rq->playerId << boundPlayerIdForClient(clientId);
        return;
    }

    trackedPlayer->skillLevels.clear();
    const int safeCount = qBound(0, rq->skillCount, MAX_SYNC_SKILL_STATE_NUM);
    for (int i = 0; i < safeCount; ++i) {
        const QString skillId = QString::fromUtf8(rq->skills[i].skillId).trimmed().toLower();
        if (skillId.isEmpty()) {
            continue;
        }
        trackedPlayer->skillLevels.insert(skillId, qMax(0, rq->skills[i].level));
    }
}

/**
 * @brief 转发RelaySkillEffect相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void core::RelaySkillEffect(quint64 clientId, STRU_SKILLFX_RQ* rq)
{
    if (!rq) {
        return;
    }

    const Player_Information* casterInfo = findBoundTrackedPlayer(clientId, rq->casterId);
    if (!casterInfo) {
        qWarning() << "Skill effect relay skipped because client binding mismatched:"
                   << clientId << rq->casterId << boundPlayerIdForClient(clientId);
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    pruneExpiredSkillHitWindows(nowMs);

    const QString skillId = normalizedSkillId(rq->skillId);
    if (rq->stage == _skill_effect_stage_impact
        && !skillId.isEmpty()
        && rq->rectW > 0.0f
        && rq->rectH > 0.0f)
    {
        ServerSkillHitWindow window;
        window.mapId = casterInfo->mapId;
        window.instanceId = casterInfo->instanceId;
        window.skillId = skillId;
        window.rect = QRectF(rq->rectX, rq->rectY, rq->rectW, rq->rectH);
        window.persistent = rq->userInts[2] > 0 && rq->userInts[3] > 0;
        const qint64 ttlMs = window.persistent ? qMax<qint64>(120, rq->userInts[2]) : 450;
        window.expiresAtMs = nowMs + ttlMs;
        m_skillHitWindows.insert(buildSkillHitWindowKey(casterInfo->player_UserId, skillId), window);
    }

    STRU_SKILLFX_RS rs{};
    rs.casterId = rq->casterId;
    rs.effectInstanceId = rq->effectInstanceId;
    rs.syncSeed = rq->syncSeed;
    rs.stage = rq->stage;
    rs.syncFlags = static_cast<quint16>(rq->syncFlags | _skill_effect_flag_remote);
    rs.serverTickMs = nowMs;
    rs.dir = rq->dir;
    rs.originX = rq->originX;
    rs.originY = rq->originY;
    rs.rectX = rq->rectX;
    rs.rectY = rq->rectY;
    rs.rectW = rq->rectW;
    rs.rectH = rq->rectH;
    std::memcpy(rs.skillId, rq->skillId, sizeof(rs.skillId));
    std::memcpy(rs.effectKey, rq->effectKey, sizeof(rs.effectKey));
    std::memcpy(rs.userInts, rq->userInts, sizeof(rs.userInts));
    std::memcpy(rs.userFloats, rq->userFloats, sizeof(rs.userFloats));

    const QByteArray packet = PacketBuilder::build(_default_protocol_skillfx_rs, rs);
    const QVector<quint64> scopedClients = collectScopedClientIds(casterInfo->mapId, casterInfo->instanceId);
    for (quint64 cid : scopedClients) {
        if (cid == clientId) {
            continue;
        }
        if (m_pKcpNet && m_pKcpNet->isKcpConnected(cid)) {
            emit sendToClientKcp(cid, packet);
        } else {
            emit sendToClient(cid, packet);
        }
    }
}

/**
 * @brief 处理calculateDamage相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
int core::calculateDamage(int attackedId, int targetId, int clientDamage)
{
    // // 获取攻击者和目标的属性
    // PlayerAttributes attackerAttributes = getPlayerAttributesFromDB(attackedId);
    // PlayerAttributes targetAttributes = getPlayerAttributesFromDB(targetId);

    // // 如果任何一个玩家的属性失败，返回0伤害
    // if (attackerAttributes.health <= 0 || targetAttributes.health <= 0) {
    //     return 0;
    // }

    // // 基础伤害 = 客户端计算的伤害 + 攻击者的攻击力
    // int baseDamage = clientDamage + attackerAttributes.attackPower;

    // // 计算目标的防御值，减少伤害
    // int reducedDamage = baseDamage - targetAttributes.defense;
    // if (reducedDamage < 0) {
    //     reducedDamage = 0;  // 防止伤害为负数
    // }

    // // 判断是否暴击，暴击率计算
    // float critChance = attackerAttributes.critRate;
    // bool isCrit = ((rand() % 100) < (critChance * 100));  // 暴击发生的概率（比如暴击率是20%，则概率是20%）
    // if (isCrit) {
    //     // 暴击伤害，假设暴击伤害为2倍
    //     reducedDamage = reducedDamage * attackerAttributes.critDamage;
    //     qDebug() << "暴击！伤害倍增！";
    // }

    // // 最终伤害 = 最大0和计算的伤害
    // return std::max(0, reducedDamage);
    Q_UNUSED(attackedId);
    Q_UNUSED(targetId);
    return qMax(0, clientDamage);
}

// Player_Information core::getPlayerAttributesFromDB(int userId) {
//     // char szsql[MAXSIZE] = {0};
//     // list<string> liststr;
//     // PlayerAttributes attributes;

//     // // 查询所有需要的字段
//     // sprintf(szsql,
//     //         "SELECT u_health, u_attackpower, u_defence, u_critrate, u_critdamage, "
//     //         "u_attackrange, u_experience, u_level, u_position_x, u_position_y "
//     //         "FROM user_basic_information WHERE u_id = %d;", userId);

//     // sql->SelectMySql(szsql, 10, liststr);

//     // if (liststr.size() < 10) {
//     //     qWarning() << "获取玩家" << userId << "的属性失败，数据不完整";
//     //     return attributes; // 返回默认值
//     // }

//     // // 解析数据
//     // attributes.health = std::stoi(liststr.front()); liststr.pop_front();
//     // attributes.attackPower = std::stoi(liststr.front()); liststr.pop_front();
//     // attributes.defense = std::stoi(liststr.front()); liststr.pop_front();
//     // attributes.critRate = std::stof(liststr.front()); liststr.pop_front();
//     // attributes.critDamage = std::stof(liststr.front()); liststr.pop_front();
//     // attributes.attackRange = std::stof(liststr.front()); liststr.pop_front();
//     // attributes.experience = std::stoll(liststr.front()); liststr.pop_front();
//     // attributes.level = std::stoi(liststr.front()); liststr.pop_front();
//     // attributes.positionX = std::stof(liststr.front()); liststr.pop_front();
//     // attributes.positionY = std::stof(liststr.front()); liststr.pop_front();
//     // // attributes.state = static_cast<PlayerState>(std::stof(liststr.front()));
//     // // liststr.pop_front();
//     // //attributes.ghostEndTime = std::stoi(liststr.front()); liststr.pop_front();

//     // return attributes;
// }

/**
 * @brief 获取getHealth相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
int core::getHealth(int userId) {
    CMySql sql;
    std::list<std::string> liststr;
    if (!selectPreparedQuery(sql,
                             QStringLiteral("SELECT u_health FROM user_basic_information WHERE u_id = ?;"),
                             sqlParams(userId),
                             1,
                             liststr)
        || liststr.empty()) {
        qWarning() << "获取玩家" << userId << "血量失败";
        return -1;
    }
    return std::stoi(liststr.front());
}

/**
 * @brief 更新updateHealthInDB相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
bool core:: updateHealthInDB(int userId, int newHealth) {

    CMySql sql;
    if (!updatePreparedQuery(sql,
                             QStringLiteral("UPDATE user_basic_information SET u_health = ? WHERE u_id = ?;"),
                             sqlParams(newHealth, userId))) {
        qCritical() << "更新玩家" << userId << "血量失败";
        return false;
    }
    return true;
}
