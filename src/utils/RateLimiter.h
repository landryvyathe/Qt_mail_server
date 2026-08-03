#ifndef RATELIMITER_H
#define RATELIMITER_H

#include <QHash>
#include <QHostAddress>
#include <QDateTime>
#include <QMutex>
#include <QString>

/**
 * Thread-safe, in-memory rate limiter for per-IP tracking.
 */
class RateLimiter {
public:
    RateLimiter(int maxPerWindow, int windowSecs)
        : m_maxPerWindow(maxPerWindow), m_windowSecs(windowSecs) {}

    /**
     * Returns true if the action is allowed for this IP.
     */
    bool allow(const QHostAddress& ip) {
        QMutexLocker lock(&m_mutex);
        qint64 now = QDateTime::currentSecsSinceEpoch();
        qint64 cutoff = now - m_windowSecs;

        Window& w = m_windows[ip.toString()];
        // prune expired entries
        w.entries.erase(std::remove_if(w.entries.begin(), w.entries.end(),
                                       [cutoff](qint64 ts) { return ts <= cutoff; }),
                        w.entries.end());

        if (w.entries.size() >= static_cast<size_t>(m_maxPerWindow))
            return false;

        w.entries.push_back(now);
        return true;
    }

    /**
     * Track an authentication attempt; returns false if blocked.
     */
    bool allowAuthAttempt(const QHostAddress& ip) {
        return allow(ip);
    }

    /**
     * Reset an IP's counters (e.g. on successful auth).
     */
    void reset(const QHostAddress& ip) {
        QMutexLocker lock(&m_mutex);
        m_windows.remove(ip.toString());
    }

private:
    struct Window {
        QList<qint64> entries;
    };

    int m_maxPerWindow;
    int m_windowSecs;
    QHash<QString, Window> m_windows;
    QMutex m_mutex;
};

#endif // RATELIMITER_H
