#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace qcbridge {

// Issue #20: the Board rejoins the Wi-Fi network on its own after the AP
// disappears or the station link drops, with no reset. This class is only
// the policy - when the next join is due - fed with uptime values, so it can
// be tested without a Wi-Fi driver (tests/wifi_rejoin). QcWifiWebTransport
// feeds it the station's events and issues the joins it asks for.
//
// Why a watchdog and not "rejoin on the disconnect event": the NXP stack in
// the pinned Zephyr v4.5.0 tree re-associates on its own after a link loss
// (wlan.reassoc_control in wifi_nxp/wlcmgr/wlan.c), reporting every failed
// attempt as a failed NET_EVENT_WIFI_CONNECT_RESULT, and gives up for good
// after CONFIG_NXP_WIFI_MAX_RECONNECT_LIMIT attempts. A link loss itself
// (WLAN_REASON_LINK_LOST) raises no Zephyr event at all
// (drivers/wifi/nxp/src/nxp_wifi_drv.c). So there is no single event that
// means "the driver has stopped trying": the policy instead keeps a
// deadline that only a successful connection clears.
//
// - A drop from connected waits graceMs before the first rejoin, because
//   NET_REQUEST_WIFI_CONNECT starts with wlan_disconnect() and would tear
//   down a re-association of the driver's own that may be about to succeed.
// - Every join, the first one included, is followed by a retry after the
//   current backoff unless the station connects first. That also covers a
//   join whose result event never arrives.
// - Failure events in between never bring a join forward, so an AP that is
//   gone isn't scanned for in a tight loop.
//
// Not thread-safe: the caller serialises access (QcWifiWebTransport holds a
// spinlock, since events arrive on the net_mgmt thread and joins are issued
// from its worker thread).
class QcWifiRejoin {
public:
    struct Timing {
        // How long a dropped link is left to the driver's own re-association.
        int64_t graceMs;
        // The first retry interval after a join; it doubles up to maxRetryMs.
        int64_t firstRetryMs;
        int64_t maxRetryMs;
    };

    explicit constexpr QcWifiRejoin(Timing timing) : timing_(timing) {}

    // A join request (NET_REQUEST_WIFI_CONNECT) was just issued.
    void joinIssued(int64_t nowMs) {
        joined_ = true;
        connected_ = false;
        nextJoinAtMs_ = nowMs + retryIntervalMs();
        if (attempt_ < kMaxAttemptCount) {
            ++attempt_;
        }
    }

    // The station associated (a successful NET_EVENT_WIFI_CONNECT_RESULT).
    void connected() {
        connected_ = true;
        attempt_ = 0;
        nextJoinAtMs_.reset();
    }

    // The station isn't connected: a failed connect result or a disconnect
    // result. Only a drop from connected changes the deadline.
    void lost(int64_t nowMs) {
        if (!joined_) {
            return;
        }
        if (connected_) {
            connected_ = false;
            attempt_ = 0;
            nextJoinAtMs_ = nowMs + timing_.graceMs;
        }
    }

    [[nodiscard]] bool joinDue(int64_t nowMs) const {
        return nextJoinAtMs_.has_value() && nowMs >= *nextJoinAtMs_;
    }

    // When the next join is due, or nullopt while connected (or before the
    // first join).
    [[nodiscard]] std::optional<int64_t> nextJoinAt() const { return nextJoinAtMs_; }

private:
    // Enough doublings to reach any sane cap, without overflowing the shift.
    static constexpr int kMaxAttemptCount = 16;

    [[nodiscard]] int64_t retryIntervalMs() const {
        return std::min(timing_.firstRetryMs << attempt_, timing_.maxRetryMs);
    }

    Timing timing_;
    bool joined_ = false;
    bool connected_ = false;
    int attempt_ = 0;
    std::optional<int64_t> nextJoinAtMs_;
};

}  // namespace qcbridge
