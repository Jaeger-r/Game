#include "servermonitorbridge.h"

/**
 * @brief 获取instance相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
ServerMonitorBridge* ServerMonitorBridge::instance()
{
    static ServerMonitorBridge bridge;
    return &bridge;
}

/**
 * @brief 构造ServerMonitorBridge对象并完成基础初始化
 * @author Jaeger
 * @date 2025.3.28
 */
ServerMonitorBridge::ServerMonitorBridge(QObject* parent)
    : QObject(parent)
{
}

/**
 * @brief 查询recentLogs相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
QStringList ServerMonitorBridge::recentLogs() const
{
    QMutexLocker locker(&m_mutex);
    return m_recentLogs;
}

/**
 * @brief 追加appendLogLine相关逻辑
 * @author Jaeger
 * @date 2025.3.28
 */
void ServerMonitorBridge::appendLogLine(const QString& line)
{
    {
        QMutexLocker locker(&m_mutex);
        m_recentLogs.append(line);
        while (m_recentLogs.size() > kMaxLogLines) {
            m_recentLogs.removeFirst();
        }
    }
    emit logAdded(line);
}
