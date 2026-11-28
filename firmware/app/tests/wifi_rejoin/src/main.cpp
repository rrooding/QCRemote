// QcWifiRejoin (issue #20): when the station link is lost, the Board rejoins
// the network on its own, with no reset. These cases pin the policy down
// against uptime values alone - no Wi-Fi driver, no clock.

#include <cstdint>
#include <optional>

#include <zephyr/ztest.h>

#include "QcWifiRejoin.hpp"

using qcbridge::QcWifiRejoin;

ZTEST_SUITE(wifi_rejoin, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr QcWifiRejoin::Timing kTiming{
    .graceMs = 20'000,
    .firstRetryMs = 15'000,
    .maxRetryMs = 60'000,
};

// Issues every join that is due at `now`, as the transport's worker does,
// and returns how many it issued.
int pump(QcWifiRejoin& rejoin, int64_t now) {
    int issued = 0;
    while (rejoin.joinDue(now)) {
        rejoin.joinIssued(now);
        ++issued;
    }
    return issued;
}

// A station that joined at t=0 and was connected at t=1 s.
QcWifiRejoin connectedAtOneSecond() {
    QcWifiRejoin rejoin(kTiming);
    rejoin.joinIssued(0);
    rejoin.connected();
    return rejoin;
}

}  // namespace

ZTEST(wifi_rejoin, test_nothing_is_due_before_the_first_join)
{
    QcWifiRejoin rejoin(kTiming);
    zassert_false(rejoin.joinDue(0));
    zassert_false(rejoin.joinDue(10'000'000));
    zassert_false(rejoin.nextJoinAt().has_value());
}

ZTEST(wifi_rejoin, test_a_connected_station_never_rejoins)
{
    QcWifiRejoin rejoin = connectedAtOneSecond();
    zassert_false(rejoin.joinDue(10'000'000));
    zassert_false(rejoin.nextJoinAt().has_value());
}

ZTEST(wifi_rejoin, test_a_join_that_never_reports_back_is_retried)
{
    // No result event at all (the driver lost the request): the first join
    // is retried after firstRetryMs anyway.
    QcWifiRejoin rejoin(kTiming);
    rejoin.joinIssued(0);

    zassert_false(rejoin.joinDue(14'999));
    zassert_true(rejoin.joinDue(15'000));
}

ZTEST(wifi_rejoin, test_a_dropped_link_waits_out_the_grace_period_first)
{
    // The NXP stack re-associates on its own after a link loss, and
    // NET_REQUEST_WIFI_CONNECT starts with wlan_disconnect(), so joining
    // straight away would tear down a re-association that may be about to
    // succeed.
    QcWifiRejoin rejoin = connectedAtOneSecond();
    rejoin.lost(100'000);

    zassert_false(rejoin.joinDue(119'999));
    zassert_true(rejoin.joinDue(120'000));
}

ZTEST(wifi_rejoin, test_the_drivers_own_reassociation_succeeding_cancels_the_rejoin)
{
    QcWifiRejoin rejoin = connectedAtOneSecond();
    rejoin.lost(100'000);
    rejoin.connected();

    zassert_false(rejoin.joinDue(1'000'000));
}

ZTEST(wifi_rejoin, test_failures_during_the_grace_period_do_not_cut_it_short)
{
    // While the driver re-associates it reports each failed attempt as a
    // failed connect result; none of them may bring the rejoin forward.
    QcWifiRejoin rejoin = connectedAtOneSecond();
    rejoin.lost(100'000);
    rejoin.lost(105'000);
    rejoin.lost(110'000);

    zassert_false(rejoin.joinDue(119'999));
    zassert_true(rejoin.joinDue(120'000));
}

ZTEST(wifi_rejoin, test_rejoins_back_off_and_are_capped)
{
    QcWifiRejoin rejoin = connectedAtOneSecond();
    rejoin.lost(0);

    int64_t now = 20'000;
    zassert_equal(pump(rejoin, now), 1);
    const int64_t expectedGaps[] = {15'000, 30'000, 60'000, 60'000, 60'000};
    for (const int64_t gap : expectedGaps) {
        zassert_equal(rejoin.nextJoinAt().value(), now + gap, "gap %lld", (long long)gap);
        zassert_false(rejoin.joinDue(now + gap - 1));
        now += gap;
        zassert_equal(pump(rejoin, now), 1);
    }
}

ZTEST(wifi_rejoin, test_a_failed_rejoin_does_not_retry_before_its_backoff)
{
    // A rejoin's own failure result arrives seconds after the request; the
    // next one still waits for the backoff, so an AP that is gone isn't
    // scanned for in a tight loop.
    QcWifiRejoin rejoin = connectedAtOneSecond();
    rejoin.lost(0);
    zassert_equal(pump(rejoin, 20'000), 1);

    rejoin.lost(23'000);

    zassert_false(rejoin.joinDue(34'999));
    zassert_true(rejoin.joinDue(35'000));
}

ZTEST(wifi_rejoin, test_reconnecting_starts_the_backoff_over)
{
    QcWifiRejoin rejoin = connectedAtOneSecond();
    rejoin.lost(0);
    pump(rejoin, 20'000);
    pump(rejoin, 35'000);
    pump(rejoin, 65'000);
    rejoin.connected();

    rejoin.lost(1'000'000);
    zassert_equal(pump(rejoin, 1'020'000), 1);
    zassert_equal(rejoin.nextJoinAt().value(), 1'035'000);
}

ZTEST(wifi_rejoin, test_a_first_join_that_fails_is_retried_without_a_grace_period)
{
    // Never connected yet: there is no re-association of the driver's own
    // to wait for, so the failed first join is just retried on the backoff.
    QcWifiRejoin rejoin(kTiming);
    rejoin.joinIssued(0);
    rejoin.lost(3'000);

    zassert_false(rejoin.joinDue(14'999));
    zassert_true(rejoin.joinDue(15'000));
}

ZTEST(wifi_rejoin, test_a_loss_before_any_join_is_ignored)
{
    // An SSID-less build never joins, so it must never start rejoining
    // because of a stray disconnect event either.
    QcWifiRejoin rejoin(kTiming);
    rejoin.lost(0);
    zassert_false(rejoin.joinDue(10'000'000));
}
