#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <optional>

namespace wayfinder {
// A burst gets three automatic retries. One good frame must not replenish the
// budget, or intermittent failures could keep rebuilding textures forever.
class RenderRecovery {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::array retryDelays{std::chrono::seconds(2), std::chrono::seconds(5), std::chrono::seconds(10)};

    bool recovering() const { return mWaiting; }
    bool stopped() const { return mWaiting && mFailures > retryDelays.size(); }
    unsigned failures() const { return mFailures; }
    bool ready(Clock::time_point now) const { return !mWaiting || (!stopped() && now >= mRetryAt); }

    void failed(Clock::time_point now) {
        mFailures = std::min(mFailures + 1, unsigned(retryDelays.size() + 1));
        mWaiting = true;
        mStableSince.reset();
        mLastSuccess.reset();
        if (!stopped()) mRetryAt = now + retryDelays[mFailures - 1];
    }
    // Returns true once when rendering has recovered, for a local notification.
    bool succeeded(Clock::time_point now) {
        bool restored = mWaiting;
        mWaiting = false;
        if (!mLastSuccess || now - *mLastSuccess > std::chrono::seconds(1)) mStableSince = now;
        mLastSuccess = now;
        if (now - *mStableSince >= std::chrono::seconds(30)) mFailures = 0;
        return restored;
    }
    void retryNow(Clock::time_point now) {
        if (!mWaiting) return;
        mFailures = 0;
        mRetryAt = now;
        mStableSince.reset();
        mLastSuccess.reset();
    }

private:
    unsigned mFailures{};
    bool mWaiting{};
    Clock::time_point mRetryAt{};
    std::optional<Clock::time_point> mStableSince, mLastSuccess;
};
} // namespace wayfinder
