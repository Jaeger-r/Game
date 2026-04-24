#include "servermonitorwindow.h"

#include <QApplication>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "core.h"
#include "servermonitorbridge.h"

namespace {
QString formatBoolState(bool ok)
{
    return ok ? QStringLiteral("OK") : QStringLiteral("FAIL");
}

QString formatPartyId(int partyId)
{
    return partyId > 0 ? QString::number(partyId) : QStringLiteral("-");
}
}

/**
 * @brief 构造ServerMonitorWindow对象并完成基础初始化
 * @author Jaeger
 * @date 2025.3.28
 */
ServerMonitorWindow::ServerMonitorWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setupUi();
    applyTheme();
    appendInitialLogs();

    connect(ServerMonitorBridge::instance(),
            &ServerMonitorBridge::logAdded,
            this,
            &ServerMonitorWindow::appendLogLine);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(1000);
    connect(m_refreshTimer, &QTimer::timeout, this, &ServerMonitorWindow::refreshSnapshot);
    m_refreshTimer->start();

    refreshSnapshot();
}

/**
 * @brief 创建createSummaryCard相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
QWidget* ServerMonitorWindow::createSummaryCard(const QString& title, QLabel*& valueLabel)
{
    QWidget* card = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(6);

    QLabel* titleLabel = new QLabel(title, card);
    titleLabel->setObjectName(QStringLiteral("monitorCardTitle"));
    valueLabel = new QLabel(QStringLiteral("--"), card);
    valueLabel->setObjectName(QStringLiteral("monitorCardValue"));

    layout->addWidget(titleLabel);
    layout->addWidget(valueLabel);
    return card;
}

/**
 * @brief 初始化setupUi相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void ServerMonitorWindow::setupUi()
{
    setWindowTitle(QStringLiteral("GameServer 可视化监控平台"));
    resize(1440, 900);

    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(18, 18, 18, 18);
    rootLayout->setSpacing(12);

    QLabel* titleLabel = new QLabel(QStringLiteral("GameServer Monitor"), central);
    titleLabel->setObjectName(QStringLiteral("monitorTitle"));
    rootLayout->addWidget(titleLabel);

    QGridLayout* cardLayout = new QGridLayout();
    cardLayout->setHorizontalSpacing(12);
    cardLayout->setVerticalSpacing(12);

    cardLayout->addWidget(createSummaryCard(QStringLiteral("整体状态"), m_statusLabel), 0, 0);
    cardLayout->addWidget(createSummaryCard(QStringLiteral("采样时间"), m_timeLabel), 0, 1);
    cardLayout->addWidget(createSummaryCard(QStringLiteral("TCP / KCP"), m_tcpLabel), 0, 2);
    cardLayout->addWidget(createSummaryCard(QStringLiteral("KCP连接"), m_kcpLabel), 0, 3);
    cardLayout->addWidget(createSummaryCard(QStringLiteral("在线玩家"), m_playersLabel), 1, 0);
    cardLayout->addWidget(createSummaryCard(QStringLiteral("队伍 / 邀请"), m_partiesLabel), 1, 1);
    cardLayout->addWidget(createSummaryCard(QStringLiteral("副本房间"), m_dungeonsLabel), 1, 2);
    cardLayout->addWidget(createSummaryCard(QStringLiteral("总包量"), m_packetsLabel), 1, 3);
    cardLayout->addWidget(createSummaryCard(QStringLiteral("逻辑耗时"), m_logicLabel), 2, 0, 1, 4);
    rootLayout->addLayout(cardLayout);

    QTabWidget* tabs = new QTabWidget(central);
    tabs->setDocumentMode(true);

    m_playersTable = new QTableWidget(0, 10, tabs);
    m_playersTable->setHorizontalHeaderLabels(
        {QStringLiteral("玩家ID"), QStringLiteral("Client"), QStringLiteral("名字"),
         QStringLiteral("等级"), QStringLiteral("地图"), QStringLiteral("实例"),
         QStringLiteral("队伍"), QStringLiteral("HP"), QStringLiteral("MP"),
         QStringLiteral("坐标")});
    m_playersTable->horizontalHeader()->setStretchLastSection(true);
    m_playersTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_playersTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_playersTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    tabs->addTab(m_playersTable, QStringLiteral("在线玩家"));

    m_partiesTable = new QTableWidget(0, 5, tabs);
    m_partiesTable->setHorizontalHeaderLabels(
        {QStringLiteral("队伍ID"), QStringLiteral("队长"), QStringLiteral("共享实例"),
         QStringLiteral("人数"), QStringLiteral("成员")});
    m_partiesTable->horizontalHeader()->setStretchLastSection(true);
    m_partiesTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_partiesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_partiesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    tabs->addTab(m_partiesTable, QStringLiteral("队伍"));

    m_dungeonsTable = new QTableWidget(0, 7, tabs);
    m_dungeonsTable->setHorizontalHeaderLabels(
        {QStringLiteral("地图"), QStringLiteral("实例"), QStringLiteral("缩放等级"),
         QStringLiteral("怪物总数"), QStringLiteral("存活"), QStringLiteral("在线玩家"),
         QStringLiteral("状态")});
    m_dungeonsTable->horizontalHeader()->setStretchLastSection(true);
    m_dungeonsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_dungeonsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_dungeonsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    tabs->addTab(m_dungeonsTable, QStringLiteral("副本"));

    m_logView = new QPlainTextEdit(tabs);
    m_logView->setReadOnly(true);
    tabs->addTab(m_logView, QStringLiteral("实时日志"));

    rootLayout->addWidget(tabs, 1);
}

/**
 * @brief 应用applyTheme相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void ServerMonitorWindow::applyTheme()
{
    qApp->setStyle(QStringLiteral("Fusion"));
    setStyleSheet(QStringLiteral(
        "QMainWindow, QWidget { background: #111827; color: #E5E7EB; }"
        "QTabWidget::pane { border: 1px solid #243041; background: #0F172A; }"
        "QTabBar::tab { background: #172033; color: #D1D5DB; padding: 10px 16px; margin-right: 4px; }"
        "QTabBar::tab:selected { background: #22314B; color: #F9FAFB; }"
        "QTableWidget, QPlainTextEdit { background: #0B1220; border: 1px solid #243041; }"
        "QHeaderView::section { background: #162036; color: #D1D5DB; padding: 6px; border: 0; }"
        "#monitorTitle { font-size: 24px; font-weight: 700; color: #F9FAFB; }"
        "#monitorCardTitle { font-size: 12px; color: #94A3B8; }"
        "#monitorCardValue { font-size: 18px; font-weight: 700; color: #F8FAFC; }"
        "QWidget { border-radius: 8px; }"));
}

/**
 * @brief 追加appendInitialLogs相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void ServerMonitorWindow::appendInitialLogs()
{
    const QStringList recentLogs = ServerMonitorBridge::instance()->recentLogs();
    if (!recentLogs.isEmpty()) {
        m_logView->setPlainText(recentLogs.join('\n'));
        m_logView->moveCursor(QTextCursor::End);
    }
}

/**
 * @brief 处理appendLogLine相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void ServerMonitorWindow::appendLogLine(const QString& line)
{
    m_logView->appendPlainText(line);
    QTextCursor cursor = m_logView->textCursor();
    cursor.movePosition(QTextCursor::End);
    m_logView->setTextCursor(cursor);
}

/**
 * @brief 填充populatePlayersTable相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void ServerMonitorWindow::populatePlayersTable(const QVector<PlayerMonitorEntry>& players)
{
    m_playersTable->setRowCount(players.size());
    for (int row = 0; row < players.size(); ++row) {
        const PlayerMonitorEntry& entry = players[row];
        const QString coordText = QStringLiteral("(%1, %2)")
                                      .arg(QString::number(entry.positionX, 'f', 1))
                                      .arg(QString::number(entry.positionY, 'f', 1));
        const QStringList columns = {
            QString::number(entry.playerId),
            QString::number(entry.clientId),
            entry.name,
            QString::number(entry.level),
            entry.mapId,
            QString::number(entry.instanceId),
            formatPartyId(entry.partyId),
            QString::number(entry.health),
            QString::number(entry.mana),
            coordText
        };
        for (int col = 0; col < columns.size(); ++col) {
            m_playersTable->setItem(row, col, new QTableWidgetItem(columns[col]));
        }
    }
}

/**
 * @brief 填充populatePartiesTable相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void ServerMonitorWindow::populatePartiesTable(const QVector<PartyMonitorEntry>& parties)
{
    m_partiesTable->setRowCount(parties.size());
    for (int row = 0; row < parties.size(); ++row) {
        const PartyMonitorEntry& entry = parties[row];
        const QStringList columns = {
            QString::number(entry.partyId),
            QStringLiteral("%1 (%2)").arg(entry.leaderName).arg(entry.leaderPlayerId),
            QString::number(entry.sharedInstanceId),
            QString::number(entry.memberCount),
            entry.memberNames
        };
        for (int col = 0; col < columns.size(); ++col) {
            m_partiesTable->setItem(row, col, new QTableWidgetItem(columns[col]));
        }
    }
}

/**
 * @brief 填充populateDungeonsTable相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void ServerMonitorWindow::populateDungeonsTable(const QVector<DungeonMonitorEntry>& dungeons)
{
    m_dungeonsTable->setRowCount(dungeons.size());
    for (int row = 0; row < dungeons.size(); ++row) {
        const DungeonMonitorEntry& entry = dungeons[row];
        const QStringList columns = {
            entry.mapId,
            QString::number(entry.instanceId),
            QString::number(entry.scaledLevel),
            QString::number(entry.totalMonsterCount),
            QString::number(entry.aliveMonsterCount),
            QString::number(entry.onlinePlayerCount),
            entry.completed ? QStringLiteral("已完成") : QStringLiteral("进行中")
        };
        for (int col = 0; col < columns.size(); ++col) {
            m_dungeonsTable->setItem(row, col, new QTableWidgetItem(columns[col]));
        }
    }
}

/**
 * @brief 刷新refreshSnapshot相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void ServerMonitorWindow::refreshSnapshot()
{
    auto* kernel = dynamic_cast<core*>(core::getKernel());
    if (!kernel) {
        return;
    }

    const ServerMonitorSnapshot snapshot = kernel->monitorSnapshot();

    m_statusLabel->setText(QStringLiteral("TCP %1 | KCP %2 | DB %3 | File %4")
                               .arg(formatBoolState(snapshot.modules.tcpOk))
                               .arg(formatBoolState(snapshot.modules.kcpOk))
                               .arg(formatBoolState(snapshot.modules.dbOk))
                               .arg(formatBoolState(snapshot.modules.fileOk)));
    m_timeLabel->setText(snapshot.capturedAt.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")));
    m_tcpLabel->setText(QStringLiteral("%1:%2 / 在线 %3")
                            .arg(snapshot.tcp.listening ? QStringLiteral("Listening") : QStringLiteral("Stopped"))
                            .arg(snapshot.tcp.port)
                            .arg(snapshot.tcp.onlineConnections));
    m_kcpLabel->setText(QStringLiteral("%1:%2 / KCP %3")
                            .arg(snapshot.kcp.listening ? QStringLiteral("Listening") : QStringLiteral("Stopped"))
                            .arg(snapshot.kcp.port)
                            .arg(snapshot.kcp.activeConnections));
    m_playersLabel->setText(QStringLiteral("%1 / %2")
                                .arg(snapshot.onlineTrackedPlayerCount)
                                .arg(snapshot.trackedPlayerCount));
    m_partiesLabel->setText(QStringLiteral("%1 队 / %2 邀请")
                                .arg(snapshot.partyCount)
                                .arg(snapshot.pendingInviteCount));
    m_dungeonsLabel->setText(QStringLiteral("%1 房 / 活跃 %2")
                                 .arg(snapshot.dungeonRoomCount)
                                 .arg(snapshot.activeDungeonRoomCount));
    m_packetsLabel->setText(QStringLiteral("总 %1 | 逻辑 %2")
                                .arg(snapshot.tcp.totalPackets)
                                .arg(snapshot.tcp.logicPackets));
    m_logicLabel->setText(QStringLiteral("active=%1 | avg=%2ms | max=%3ms")
                              .arg(snapshot.tcp.logicActive)
                              .arg(QString::number(snapshot.tcp.averageLogicMs, 'f', 2))
                              .arg(QString::number(snapshot.tcp.maxLogicMs, 'f', 2)));

    populatePlayersTable(snapshot.players);
    populatePartiesTable(snapshot.parties);
    populateDungeonsTable(snapshot.dungeons);
}
