// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_PERFORMANCE_WINDOW_H
#define CONNECTCOIN_WALLET_P2C_PERFORMANCE_WINDOW_H

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <stdexcept>

namespace wallet {

inline double P2CPerformanceNow()
{
    return std::chrono::duration<double>{std::chrono::steady_clock::now().time_since_epoch()}.count();
}

struct P2CPerformanceTotals {
    uint64_t attempts{0};
    uint64_t valid{0};
    double seconds{0};
};

/** One permanent prior, never inserted into actual observations or each bucket. */
inline double GetP2CSmoothedPerformanceRate(const P2CPerformanceTotals& totals)
{
    return std::clamp((5.0 + static_cast<double>(totals.valid)) / (1.0 + totals.seconds),
                      std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max());
}

/** Completion-time minute buckets: the current minute and the preceding 59.
 * Ordinary samples/readers touch one bucket. Empty minutes are expired once;
 * a gap of an hour clears a fixed-size array, never per-connection history.
 * The caller serializes access and supplies a monotonic clock.
 */
class P2CMinutePerformanceWindow {
    std::array<P2CPerformanceTotals, 60> m_buckets{};
    P2CPerformanceTotals m_totals{};
    int64_t m_minute{-1};
    int64_t m_observed_minute{-1};

public:
    static int64_t Minute(double seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0 || seconds / 60 >= static_cast<double>(INT64_MAX)) {
            throw std::invalid_argument("Invalid monotonic P2C performance timestamp");
        }
        return static_cast<int64_t>(std::floor(seconds / 60));
    }

    P2CPerformanceTotals ReadMinute(int64_t minute)
    {
        if (minute < 0) throw std::invalid_argument("Invalid P2C performance minute");
        // A test-clock regression cannot resurrect expired observations.
        minute = std::max(minute, m_minute);
        if (m_minute < 0 || minute - m_minute >= 60) {
            m_buckets.fill({});
            m_totals = {};
            m_minute = minute;
        } else if (minute > m_minute) {
            while (m_minute < minute) m_buckets[static_cast<size_t>(++m_minute % 60)] = {};
            // Re-sum on minute changes instead of repeatedly subtracting large
            // expired totals and losing smaller observations that remain.
            m_totals = {};
            long double seconds{0};
            for (const auto& bucket : m_buckets) {
                m_totals.attempts += bucket.attempts;
                m_totals.valid += bucket.valid;
                seconds += bucket.seconds;
            }
            m_totals.seconds = static_cast<double>(std::min(seconds, static_cast<long double>(std::numeric_limits<double>::max())));
        }
        return m_totals;
    }

    P2CPerformanceTotals Read(double now) { return ReadMinute(Minute(now)); }

    bool RecordMinute(bool valid, double seconds, int64_t minute)
    {
        if (!std::isfinite(seconds) || seconds < 0) return false;
        ReadMinute(minute);
        auto& bucket{m_buckets[static_cast<size_t>(m_minute % 60)]};
        if (m_totals.attempts == UINT64_MAX || !std::isfinite(m_totals.seconds + seconds) || !std::isfinite(bucket.seconds + seconds)) return false;
        ++bucket.attempts;
        bucket.valid += valid ? 1 : 0;
        bucket.seconds += seconds;
        ++m_totals.attempts;
        m_totals.valid += valid ? 1 : 0;
        m_totals.seconds += seconds;
        m_observed_minute = m_minute;
        return true;
    }

    bool Record(bool valid, double seconds, double now) { return RecordMinute(valid, seconds, Minute(now)); }
    int64_t ObservedMinute() const { return m_observed_minute; }
};

class P2CPerformanceCache;
struct P2CPerformanceScore;
using P2CPerformanceScoreList = std::list<std::shared_ptr<P2CPerformanceScore>>;

struct P2CPerformanceScore {
private:
    friend class P2CPerformanceCache;
    std::unique_ptr<P2CMinutePerformanceWindow> m_window;
    P2CPerformanceScoreList::iterator m_position;
    const P2CPerformanceCache* m_owner{nullptr};
};

/** Bounded, lazy histories shared by a worker's domain and endpoint scores.
 * DNS entries and in-flight work retain their score identity independently of
 * histories. Eviction/expiry restores the 5/1 prior without resetting rotation
 * credit, claim counters or bounty budgets. All callers use the worker mutex.
 */
class P2CPerformanceCache {
    const size_t m_maximum;
    P2CPerformanceScoreList m_recent;
    int64_t m_minute{-1};

    void Remove(const std::shared_ptr<P2CPerformanceScore>& score)
    {
        // The argument may refer to the node being erased.
        const auto retained{score};
        m_recent.erase(retained->m_position);
        retained->m_window.reset();
        retained->m_owner = nullptr;
    }

    int64_t Advance(double now)
    {
        m_minute = std::max(m_minute, P2CMinutePerformanceWindow::Minute(now));
        // Ordered by the last conclusive sample. Free each stale history once,
        // without scanning the DNS table/cache on the per-connection hot path.
        while (!m_recent.empty() && m_minute - m_recent.front()->m_window->ObservedMinute() >= 60) Remove(m_recent.front());
        return m_minute;
    }

    void CheckOwner(const std::shared_ptr<P2CPerformanceScore>& score) const
    {
        if (!score || (score->m_owner && score->m_owner != this)) throw std::invalid_argument("P2C score belongs to another performance cache");
    }

public:
    static constexpr size_t DEFAULT_MAXIMUM{8192};
    explicit P2CPerformanceCache(size_t maximum = DEFAULT_MAXIMUM) : m_maximum(maximum)
    {
        if (!maximum) throw std::invalid_argument("P2C performance cache requires a positive bound");
    }
    P2CPerformanceCache(const P2CPerformanceCache&) = delete;
    P2CPerformanceCache& operator=(const P2CPerformanceCache&) = delete;
    ~P2CPerformanceCache()
    {
        for (const auto& score : m_recent) {
            score->m_window.reset();
            score->m_owner = nullptr;
        }
    }

    size_t Size() const { return m_recent.size(); }

    double Rate(const std::shared_ptr<P2CPerformanceScore>& score, double now)
    {
        CheckOwner(score);
        const auto minute{Advance(now)};
        return GetP2CSmoothedPerformanceRate(score->m_window ? score->m_window->ReadMinute(minute) : P2CPerformanceTotals{});
    }

    bool Record(const std::shared_ptr<P2CPerformanceScore>& score, bool valid, double seconds, double now)
    {
        if (!std::isfinite(seconds) || seconds < 0) return false;
        CheckOwner(score);
        const auto minute{Advance(now)};
        if (!score->m_window) {
            if (m_recent.size() == m_maximum) Remove(m_recent.front());
            auto window{std::make_unique<P2CMinutePerformanceWindow>()};
            m_recent.push_back(score);
            score->m_position = std::prev(m_recent.end());
            score->m_window = std::move(window);
            score->m_owner = this;
        }
        if (!score->m_window->RecordMinute(valid, seconds, minute)) return false;
        m_recent.splice(m_recent.end(), m_recent, score->m_position);
        return true;
    }
};

} // namespace wallet

#endif // CONNECTCOIN_WALLET_P2C_PERFORMANCE_WINDOW_H
