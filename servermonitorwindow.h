#ifndef SERVERMONITORWINDOW_H
#define SERVERMONITORWINDOW_H

#include <QMainWindow>

#include "servermonitortypes.h"

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QPlainTextEdit)
QT_FORWARD_DECLARE_CLASS(QTableWidget)
QT_FORWARD_DECLARE_CLASS(QTimer)

class ServerMonitorWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit ServerMonitorWindow(QWidget* parent = nullptr);

private slots:
    void refreshSnapshot();
    void appendLogLine(const QString& line);

private:
    QWidget* createSummaryCard(const QString& title, QLabel*& valueLabel);
    void setupUi();
    void applyTheme();
    void populatePlayersTable(const QVector<PlayerMonitorEntry>& players);
    void populatePartiesTable(const QVector<PartyMonitorEntry>& parties);
    void populateDungeonsTable(const QVector<DungeonMonitorEntry>& dungeons);
    void appendInitialLogs();

    QLabel* m_statusLabel = nullptr;
    QLabel* m_timeLabel = nullptr;
    QLabel* m_tcpLabel = nullptr;
    QLabel* m_kcpLabel = nullptr;
    QLabel* m_playersLabel = nullptr;
    QLabel* m_partiesLabel = nullptr;
    QLabel* m_dungeonsLabel = nullptr;
    QLabel* m_packetsLabel = nullptr;
    QLabel* m_logicLabel = nullptr;

    QTableWidget* m_playersTable = nullptr;
    QTableWidget* m_partiesTable = nullptr;
    QTableWidget* m_dungeonsTable = nullptr;
    QPlainTextEdit* m_logView = nullptr;
    QTimer* m_refreshTimer = nullptr;
};

#endif // SERVERMONITORWINDOW_H
