#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/websocket.h>
#include <zephyr/net/wifi_mgmt.h>

#include <etl/vector.h>

#include "QcCatalogJson.hpp"
#include "QcCatalogSnapshot.hpp"
#include "QcPresetTransport.hpp"
#include "ScopedKMutex.hpp"

namespace qcbridge {

// HTTP_SERVICE_DEFINE (below, at namespace scope) needs a plain uint16_t*
// for its port, and QcWifiWebTransport's own logStartupSummary()/
// logReadySummary() want to log that same port - declared up here, before
// the class, so both can see it (a member function body can't reference
// a namespace-scope name declared later in the same file).
inline uint16_t g_qcWebPort = 80;

// Issue #71's concrete Wi-Fi/web transport: implements QcPresetTransport by
// serving a small embedded page and a WebSocket over Zephyr's HTTP server
// (zephyr/net/http/server.h), on top of a Wi-Fi interface brought up either
// as an access point or a station, per CONFIG_WIFI_USAGE_MODE_AP/STA
// (chosen at build time - see Kconfig/wifi_credentials.conf.example).
//
// QcSession/QcPresetCatalog never include this header or anything it pulls
// in - they only know QcPresetTransport.hpp. This class is the only place
// in the firmware that mentions Wi-Fi or HTTP.
//
// Zephyr's HTTP_SERVICE_DEFINE/HTTP_RESOURCE_DEFINE macros (below, at
// namespace scope) generate the file-scope static tables the HTTP server
// discovers resources through - a library-interface requirement with no
// way around it, same exception CODING_STANDARDS.md already carves out for
// Zephyr's own C APIs (see UsbHostClass.hpp/usbh_shim.c). Those macros'
// callbacks are plain C function pointers, not methods, so they go through
// a single static instance pointer (instance_) rather than threading a
// capturing closure through - legitimate here specifically because exactly
// one QcWifiWebTransport is ever constructed (Main.cpp), not a general
// singleton pattern for this class.
class QcWifiWebTransport : public QcPresetTransport {
public:
    QcWifiWebTransport() {
        k_mutex_init(&dataMutex_);
        k_sem_init(&broadcastSem_, 0, 1);
        instance_ = this;

        // PR #74 review: before the first File-stream listing finishes (or
        // indefinitely without a Quad Cortex attached), there's nothing to
        // serve yet. An empty HTTP body makes the page's `r.json()` reject
        // and leaves it stuck at "loading..." - encode a real, valid,
        // explicitly-incomplete catalog instead of leaving the buffer
        // empty, through the same tested encoder every other catalog goes
        // through (tests/preset_transport), not a hand-written literal.
        catalogSnapshot_.incomplete = true;
        encodeCatalogJson(catalogSnapshot_, catalogJson_);
    }

    // Brings up Wi-Fi, starts Zephyr's HTTP server, and starts the worker
    // thread that pushes updates to connected websocket clients. Call once
    // at startup (Main.cpp), before QcSession::addTransport(*this).
    void start() {
        net_mgmt_init_event_callback(&netMgmtCb_, &onWifiEventTrampoline, kNetEventMask);
        net_mgmt_add_event_callback(&netMgmtCb_);

        // PR #74 review: HTTP_SERVICE_DEFINE/HTTP_RESOURCE_DEFINE only
        // register the resources - Zephyr's HTTP server thread waits for
        // an explicit http_server_start() before it ever accepts a
        // connection. Without this call neither AP nor station mode could
        // ever serve the page or accept a WebSocket, regardless of Wi-Fi
        // coming up correctly.
        const int httpRet = http_server_start();
        httpServerStarted_ = (httpRet == 0);
        if (!httpServerStarted_) {
            LOG_ERR("http_server_start() failed: %d - the page and WebSocket will be "
                    "unreachable even if Wi-Fi comes up",
                    httpRet);
        }

#if defined(CONFIG_WIFI_USAGE_MODE_AP)
        startAccessPoint();
#else
        startStation();
#endif

        if (!threadStarted_) {
            k_thread_create(&thread_, stack_, K_KERNEL_STACK_SIZEOF(stack_), &threadEntryTrampoline,
                            this, nullptr, nullptr, kThreadPriority, 0, K_NO_WAIT);
            k_thread_name_set(&thread_, "qc_wifi_web");
            threadStarted_ = true;
        }

        logStartupSummary();
    }

    // QcPresetTransport overrides - called from QcSession's own heartbeat/
    // dispatcher threads (see that header's contract). Both just copy into
    // member storage and signal the worker thread; the actual JSON
    // encoding and socket I/O (either of which can take real time - PR #74
    // review flagged encoding running on qc_heartbeat as a KeepAlive-path
    // regression) only ever happens on qc_wifi_web, never here.
    void publishCatalog(const QcCatalogSnapshot& snapshot) override {
        {
            ScopedKMutex lock(dataMutex_);
            catalogSnapshot_ = snapshot;
            catalogDirty_ = true;
        }
        k_sem_give(&broadcastSem_);
    }

    void publishPresetChange(const QcPresetChangeEvent& event) override {
        // PR #74 review: silently rebroadcasting the stored catalog JSON
        // verbatim made a preset switch invisible on the page whenever it
        // didn't also change the folder/preset list (render() would show
        // the same names/count/generation as before). broadcast() now
        // folds this event's command/count into the next message it sends
        // as a "lastChange" field alongside the catalog, so the page can
        // show something changed even when the listing itself didn't.
        {
            ScopedKMutex lock(dataMutex_);
            lastChangeEvent_ = event;
            changePending_ = true;
        }
        k_sem_give(&broadcastSem_);
    }

    // HTTP server callbacks (registered via HTTP_RESOURCE_DEFINE below).
    // Public only because Zephyr's resource-definition macros need a
    // plain function pointer at namespace scope - see this class's own
    // header comment on why they route through instance_ rather than a
    // bound closure.
    static int handlePresetsGet(http_client_ctx* /*client*/, http_transaction_status status,
                                const http_request_ctx* /*request_ctx*/,
                                http_response_ctx* response_ctx, void* /*user_data*/) {
        if (instance_ == nullptr || status != HTTP_SERVER_REQUEST_DATA_FINAL) {
            return 0;
        }
        // PR #74 review: Zephyr sends response_ctx->body after this
        // callback returns, by which point dataMutex_ has already been
        // released - a concurrent publishCatalog() re-encoding catalogJson_
        // mid-send would otherwise corrupt this response. responseJson_ is
        // this class's own copy, touched only from the single-threaded
        // Zephyr HTTP server thread (K_THREAD_DEFINE(http_server_tid, ...),
        // confirmed against subsys/net/lib/http/http_server_core.c - one
        // resource callback runs at a time), so it stays stable for the
        // duration of this response without holding dataMutex_ across the
        // actual socket send.
        {
            ScopedKMutex lock(instance_->dataMutex_);
            instance_->responseJson_.assign(instance_->catalogJson_.begin(),
                                            instance_->catalogJson_.end());
        }
        response_ctx->body = reinterpret_cast<const uint8_t*>(instance_->responseJson_.data());
        response_ctx->body_len = instance_->responseJson_.size();
        response_ctx->final_chunk = true;
        return 0;
    }

    static int handleWsSetup(int wsSocket, http_request_ctx* /*request_ctx*/, void* /*user_data*/) {
        if (instance_ != nullptr) {
            instance_->addWsClient(wsSocket);
            LOG_INF("Websocket client connected (sock=%d)", wsSocket);
        }
        return 0;
    }

private:
    static constexpr int kThreadPriority = 10;
    static constexpr size_t kJsonCapacity = 16384;
    static constexpr size_t kMaxWsClients = 2;
    // How often run() wakes even without a publish, to poll connected
    // sockets for control frames/EOF (PR #74 review: the worker only ever
    // woke on a broadcast, so a closed browser tab's socket was never
    // drained or unregistered until the next unrelated publish happened to
    // fail a send to it).
    static constexpr int kPollIntervalMs = 1000;

    static constexpr uint64_t kNetEventMask =
        (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT |
         NET_EVENT_WIFI_AP_ENABLE_RESULT | NET_EVENT_WIFI_AP_DISABLE_RESULT |
         NET_EVENT_WIFI_AP_STA_CONNECTED | NET_EVENT_WIFI_AP_STA_DISCONNECTED);

    static void threadEntryTrampoline(void* p1, void*, void*) {
        static_cast<QcWifiWebTransport*>(p1)->run();
    }

    void run() {
        while (true) {
            const int took = k_sem_take(&broadcastSem_, K_MSEC(kPollIntervalMs));
            if (took == 0) {
                broadcast();
            }
            pollWsClients();
        }
    }

    // Encodes the latest snapshot (if publishCatalog() ran since the last
    // broadcast) into catalogJson_ - moved here from publishCatalog() per
    // PR #74 review, so the variable-length formatting work never runs on
    // qc_heartbeat. Then copies into broadcastJson_ (never holding
    // dataMutex_ across the actual socket sends below - same split as
    // before) and pushes it to every connected client, folding in a
    // pending preset-change event as a "lastChange" field when one hasn't
    // been delivered yet.
    void broadcast() {
        QcPresetChangeEvent change{};
        bool includeChange = false;
        size_t length = 0;
        etl::vector<int, kMaxWsClients> sockets;
        {
            ScopedKMutex lock(dataMutex_);
            if (catalogDirty_) {
                encodeCatalogJson(catalogSnapshot_, catalogJson_);
                catalogDirty_ = false;
            }
            if (changePending_) {
                change = lastChangeEvent_;
                includeChange = true;
                changePending_ = false;
            }
            broadcastJson_.assign(catalogJson_.begin(), catalogJson_.end());
            if (includeChange) {
                appendLastChange(change, broadcastJson_);
            }
            length = broadcastJson_.size();
            sockets = wsSockets_;
        }

        for (const int sock : sockets) {
            // Bounded timeout (not SYS_FOREVER_MS): a wedged client must
            // only ever delay this thread's own next broadcast, never
            // block it forever - this thread has no effect on usbh/
            // qc_session/qc_heartbeat either way (see this class's own
            // header comment), but an unbounded wait here would still
            // starve the *other* connected client's updates.
            const int sent = websocket_send_msg(sock, reinterpret_cast<const uint8_t*>(broadcastJson_.data()),
                                                length, WEBSOCKET_OPCODE_DATA_TEXT,
                                                /*mask=*/false, /*final=*/true, 2000);
            if (sent < 0) {
                LOG_INF("Websocket client %d gone (err=%d), dropping", sock, sent);
                removeWsClient(sock);
                (void)websocket_unregister(sock);
            }
        }
    }

    // Rewrites broadcastJson_'s trailing "}" into
    // ,"lastChange":{"command":N,"count":M}} - broadcastJson_ is always a
    // complete, non-empty JSON object at this point (encodeCatalogJson()'s
    // own guarantee), so it always ends in "}".
    template <size_t N>
    static void appendLastChange(const QcPresetChangeEvent& event, etl::vector<char, N>& out) {
        if (!out.empty()) {
            out.pop_back();
        }
        detail::appendLiteral(",\"lastChange\":{\"command\":", out);
        detail::appendUInt(event.command, out);
        detail::appendLiteral(",\"count\":", out);
        detail::appendUInt(event.count, out);
        detail::appendLiteral("}}", out);
    }

    // PR #74 review: this worker previously only ever woke to broadcast,
    // so Zephyr handed it an upgraded socket (handleWsSetup()) that was
    // then never read from again - a closed browser tab left its slot
    // occupied (both of kMaxWsClients' worth) until an unrelated broadcast
    // happened to fail a send to it. Runs every run() wake (see
    // kPollIntervalMs), draining any pending frame per connected socket
    // without blocking, replying to pings, and unregistering anything
    // closed or erroring.
    void pollWsClients() {
        etl::vector<int, kMaxWsClients> sockets;
        {
            ScopedKMutex lock(dataMutex_);
            sockets = wsSockets_;
        }

        for (const int sock : sockets) {
            uint32_t messageType = 0;
            uint64_t remaining = 0;
            // websocket_recv_msg()'s timeout is a plain millisecond count
            // (int32_t), not a k_timeout_t - 0 means "don't block" (it
            // becomes K_MSEC(0) internally), same as every other call
            // below that passes a millisecond literal.
            const int received = websocket_recv_msg(sock, wsRecvBuffer_, sizeof(wsRecvBuffer_),
                                                     &messageType, &remaining, 0);
            if (received == -EAGAIN) {
                continue;  // Nothing pending - the common case.
            }
            if (received < 0) {
                LOG_INF("Websocket client %d closed (err=%d), dropping", sock, received);
                removeWsClient(sock);
                (void)websocket_unregister(sock);
                continue;
            }
            if (messageType & WEBSOCKET_FLAG_CLOSE) {
                LOG_INF("Websocket client %d sent close, dropping", sock);
                removeWsClient(sock);
                (void)websocket_unregister(sock);
            } else if (messageType & WEBSOCKET_FLAG_PING) {
                (void)websocket_send_msg(sock, wsRecvBuffer_, static_cast<size_t>(received),
                                         WEBSOCKET_OPCODE_PONG, /*mask=*/false, /*final=*/true,
                                         2000);
            }
            // Any other inbound frame (the page never sends data messages
            // today) is simply drained above and otherwise ignored.
        }
    }

    void addWsClient(int sock) {
        ScopedKMutex lock(dataMutex_);
        if (wsSockets_.full()) {
            LOG_WRN("Max websocket clients (%zu) reached, rejecting", kMaxWsClients);
            return;
        }
        wsSockets_.push_back(sock);
    }

    void removeWsClient(int sock) {
        ScopedKMutex lock(dataMutex_);
        const auto it = std::find(wsSockets_.begin(), wsSockets_.end(), sock);
        if (it != wsSockets_.end()) {
            wsSockets_.erase(it);
        }
    }

    // Hardware finding (ac601b2): the NXP Wi-Fi driver DOES auto-start its
    // own DHCPv4 server for SoftAP - CONFIG_NXP_WIFI_SOFTAP_DHCP_SERVER
    // defaults to y whenever CONFIG_NXP_WIFI_SOFTAP_SUPPORT is on (which
    // itself defaults to y under CONFIG_WIFI_USAGE_MODE_AP), confirmed at
    // drivers/wifi/nxp/src/nxp_wifi_drv.c's WLAN_REASON_UAP_SUCCESS case
    // (net_dhcpv4_server_start(g_uap.netif, &base_addr), gated by that
    // Kconfig). ADR 0009's original claim that the driver never does this
    // was wrong - it was checked against the generic NXP Wi-Fi Kconfig
    // text, not this specific code path.
    //
    // This class used to *also* set a static IP and start its own DHCPv4
    // server on the same interface (net_if_get_wifi_sap(), the same
    // net_if the driver's own g_uap.netif resolves to - there is only one
    // SoftAP interface on this chip). That second start always failed
    // with -EALREADY ("DHCPv4 server instance already running") - the
    // exact "DHCP Server start failed" nxp_wifi error seen on hardware -
    // and because the driver's own UAP-success handler returns early on
    // that failure, it never reached the lines that set
    // s_nxp_wifi_UapActivated/raise NET_EVENT_WIFI_AP_ENABLE_RESULT either,
    // which is why no "AP enabled" line ever followed. Zephyr's own
    // samples/net/wifi/apsta_mode sample independently reaches the same
    // conclusion for this exact board: its frdm_rw612 overlay sets
    // CONFIG_WIFI_SAMPLE_DHCPV4_START=n specifically because (per that
    // option's own Kconfig help) "some vendors start the DHCPv4 server
    // after Soft AP initialization" - this is that vendor.
    //
    // Fix: don't start a second DHCPv4 server at all. The IP range is
    // configured once, via CONFIG_NXP_WIFI_SOFTAP_IP_ADDRESS/_MASK/_BASE
    // (prj.conf), and the driver brings up the static IP and DHCPv4 server
    // itself once WLAN_REASON_UAP_SUCCESS fires.
    //
    // What this does NOT establish: WLAN_REASON_UAP_SUCCESS firing (and
    // the DHCP conflict log appearing) proves the chip firmware itself
    // already reported the SoftAP as started *before* hitting this
    // conflict - so whether removing the conflict alone is sufficient to
    // make "QCRemote" visible to a scanning phone is unconfirmed from
    // source and needs a fresh hardware run with this fix applied.
    void startAccessPoint() {
#if defined(CONFIG_WIFI_USAGE_MODE_AP)
        net_if* iface = net_if_get_wifi_sap();
        if (iface == nullptr) {
            LOG_ERR("Wi-Fi AP interface not available");
            return;
        }

        static const uint8_t kSsid[] = CONFIG_QC_WIFI_AP_SSID;
        static const uint8_t kPsk[] = CONFIG_QC_WIFI_AP_PSK;
        apParams_.ssid = kSsid;
        apParams_.ssid_length = sizeof(CONFIG_QC_WIFI_AP_SSID) - 1;
        apParams_.channel = WIFI_CHANNEL_ANY;
        // SoftAP stays 2.4 GHz-only here, deliberately: the driver's own
        // ACS only auto-picks a 5 GHz channel from this band hint (see
        // nxp_wifi_drv.c's acs_band), and 5 GHz AP operation brings in
        // DFS/regulatory channel-vacating requirements this firmware does
        // nothing to handle - not the owner's decision, just this build
        // not having done that work. Station mode (startStation(), below)
        // carries no such restriction.
        apParams_.band = WIFI_FREQ_BAND_2_4_GHZ;
        if (sizeof(CONFIG_QC_WIFI_AP_PSK) > 1) {
            apParams_.psk = kPsk;
            apParams_.psk_length = sizeof(CONFIG_QC_WIFI_AP_PSK) - 1;
            apParams_.security = WIFI_SECURITY_TYPE_PSK;
        } else {
            LOG_WRN("CONFIG_QC_WIFI_AP_PSK is empty - starting an OPEN access point. Set "
                    "CONFIG_QC_WIFI_AP_PSK in a local wifi_credentials.conf (see "
                    "wifi_credentials.conf.example) for a WPA2-protected AP instead.");
            apParams_.security = WIFI_SECURITY_TYPE_NONE;
        }

        LOG_INF("Wi-Fi AP \"%s\" starting (DHCPv4 server is driver-managed - "
                "CONFIG_NXP_WIFI_SOFTAP_IP_ADDRESS=%s)",
                CONFIG_QC_WIFI_AP_SSID, CONFIG_NXP_WIFI_SOFTAP_IP_ADDRESS);
        const int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &apParams_, sizeof(apParams_));
        if (ret != 0) {
            LOG_ERR("NET_REQUEST_WIFI_AP_ENABLE failed: %d", ret);
        }
#endif
    }

    void startStation() {
#if !defined(CONFIG_WIFI_USAGE_MODE_AP)
        net_if* iface = net_if_get_wifi_sta();
        if (iface == nullptr) {
            LOG_ERR("Wi-Fi station interface not available");
            return;
        }

        if (sizeof(CONFIG_QC_WIFI_STA_SSID) <= 1) {
            LOG_WRN("CONFIG_QC_WIFI_STA_SSID is empty - not joining a network. Set "
                    "CONFIG_QC_WIFI_STA_SSID/CONFIG_QC_WIFI_STA_PSK in a local "
                    "wifi_credentials.conf (see wifi_credentials.conf.example).");
            return;
        }

        static const uint8_t kSsid[] = CONFIG_QC_WIFI_STA_SSID;
        static const uint8_t kPsk[] = CONFIG_QC_WIFI_STA_PSK;
        staParams_.ssid = kSsid;
        staParams_.ssid_length = sizeof(CONFIG_QC_WIFI_STA_SSID) - 1;
        staParams_.channel = WIFI_CHANNEL_ANY;
        // No band hint here, deliberately (ADR 0009 originally hardcoded
        // WIFI_FREQ_BAND_2_4_GHZ as "the owner's decision" - the owner
        // never asked for that; they only said 2.4 GHz reliability wasn't
        // a concern). Confirmed by reading nxp_wifi_connect()
        // (drivers/wifi/nxp/src/nxp_wifi_drv.c): the station connect path
        // never reads params->band at all - only params->channel/
        // chan_list - so this field was always a no-op for station mode
        // on this driver, never an actual 2.4 GHz-only restriction.
        staParams_.psk = kPsk;
        staParams_.psk_length = sizeof(CONFIG_QC_WIFI_STA_PSK) - 1;
        staParams_.security =
            (sizeof(CONFIG_QC_WIFI_STA_PSK) > 1) ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE;

        LOG_INF("Joining Wi-Fi network \"%s\"", CONFIG_QC_WIFI_STA_SSID);
        const int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &staParams_, sizeof(staParams_));
        if (ret != 0) {
            LOG_ERR("NET_REQUEST_WIFI_CONNECT failed: %d", ret);
        }
#endif
    }

    static void onWifiEventTrampoline(net_mgmt_event_callback* cb, uint64_t mgmtEvent, net_if* iface) {
        if (instance_ != nullptr) {
            instance_->onWifiEvent(mgmtEvent, iface, cb);
        }
    }

    // PR #74 review / hardware finding (ac601b2): this used to log
    // "connected"/"AP enabled" unconditionally on these events, regardless
    // of outcome - which is exactly why the serial console showed "Wi-Fi
    // station connected" immediately around a logged "authentication
    // failed" warning on real hardware: the driver raises
    // NET_EVENT_WIFI_CONNECT_RESULT with a non-zero wifi_status on a
    // failed (re)association too (drivers/wifi/nxp/src/nxp_wifi_drv.c,
    // WLAN_REASON_NETWORK_AUTH_FAILED ->
    // wifi_mgmt_raise_connect_result_event(iface,
    // WIFI_STATUS_CONN_WRONG_PASSWORD)), and this handler never looked at
    // the event's own status field. Note the driver's own comment on that
    // status value: it's raised for *any* 4-way-handshake failure
    // (including lost EAPoL frames), not only an actually-wrong
    // passphrase - logging the raw status/reason codes rather than
    // renaming it is deliberate, so a real wrong password and a
    // handshake-timing failure aren't conflated in the log either.
    void onWifiEvent(uint64_t mgmtEvent, net_if* iface, net_mgmt_event_callback* cb) {
        const auto* status = static_cast<const wifi_status*>(cb->info);
        switch (mgmtEvent) {
        case NET_EVENT_WIFI_CONNECT_RESULT:
            if (status != nullptr && status->conn_status == WIFI_STATUS_CONN_SUCCESS) {
                LOG_INF("Wi-Fi station connected: %s", wifi_conn_status_txt(status->conn_status));
                logReadySummary(iface, /*isAp=*/false);
            } else {
                LOG_WRN("Wi-Fi station connect failed: %s (status_code=%u, reason_code=%u)",
                        status != nullptr ? wifi_conn_status_txt(status->conn_status) : "unknown",
                        status != nullptr ? status->status_code : 0U,
                        status != nullptr ? status->reason_code : 0U);
            }
            break;
        case NET_EVENT_WIFI_DISCONNECT_RESULT:
            LOG_INF("Wi-Fi station disconnected: %s",
                    status != nullptr ? wifi_disconn_reason_txt(status->disconn_reason) : "unknown");
            break;
        case NET_EVENT_WIFI_AP_ENABLE_RESULT:
            if (status != nullptr && status->ap_status == WIFI_STATUS_AP_SUCCESS) {
                LOG_INF("Wi-Fi AP enabled - waiting for a station to join");
                logReadySummary(iface, /*isAp=*/true);
            } else {
                LOG_ERR("Wi-Fi AP enable failed: ap_status=%d (status_code=%u, reason_code=%u)",
                        status != nullptr ? static_cast<int>(status->ap_status) : -1,
                        status != nullptr ? status->status_code : 0U,
                        status != nullptr ? status->reason_code : 0U);
            }
            break;
        case NET_EVENT_WIFI_AP_DISABLE_RESULT:
            LOG_INF("Wi-Fi AP disabled (ap_status=%d)",
                    status != nullptr ? static_cast<int>(status->ap_status) : -1);
            break;
        case NET_EVENT_WIFI_AP_STA_CONNECTED: {
            const auto* info = static_cast<const wifi_ap_sta_info*>(cb->info);
            LOG_INF("Wi-Fi AP: station %02X:%02X:%02X:%02X:%02X:%02X joined", info->mac[0],
                    info->mac[1], info->mac[2], info->mac[3], info->mac[4], info->mac[5]);
            break;
        }
        case NET_EVENT_WIFI_AP_STA_DISCONNECTED: {
            const auto* info = static_cast<const wifi_ap_sta_info*>(cb->info);
            LOG_INF("Wi-Fi AP: station %02X:%02X:%02X:%02X:%02X:%02X left", info->mac[0],
                    info->mac[1], info->mac[2], info->mac[3], info->mac[4], info->mac[5]);
            break;
        }
        default:
            break;
        }
    }

    // Issue #71's "add a clear startup summary" requirement: one line as
    // soon as start() has issued the AP/STA request and started the HTTP
    // server, reporting what's known synchronously (mode, SSID, HTTP
    // server start result). The interface/IP/DHCP side of the picture
    // isn't known yet at this point - logReadySummary() below reports that
    // once the relevant NET_EVENT_WIFI_* result actually arrives.
    void logStartupSummary() {
#if defined(CONFIG_WIFI_USAGE_MODE_AP)
        LOG_INF("Startup summary: mode=AP ssid=\"%s\" http_server=%s port=%u",
                CONFIG_QC_WIFI_AP_SSID, httpServerStarted_ ? "listening" : "FAILED TO START",
                g_qcWebPort);
#else
        LOG_INF("Startup summary: mode=STA ssid=\"%s\" http_server=%s port=%u",
                sizeof(CONFIG_QC_WIFI_STA_SSID) > 1 ? CONFIG_QC_WIFI_STA_SSID : "(none configured)",
                httpServerStarted_ ? "listening" : "FAILED TO START", g_qcWebPort);
#endif
    }

    // Called once the interface actually reports success (AP enabled, or
    // station connected) - reports the interface's current IPv4 address
    // (if any yet) and a ready-to-browse URL, or says plainly that no
    // address has been assigned yet.
    void logReadySummary(net_if* iface, bool isAp) {
        const net_in_addr* addr =
            iface != nullptr ? net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) : nullptr;
        char ipStr[NET_IPV4_ADDR_LEN] = "none yet";
        if (addr != nullptr) {
            net_addr_ntop(NET_AF_INET, addr, ipStr, sizeof(ipStr));
        }
        LOG_INF("Startup summary: mode=%s iface=%s ip=%s dhcp=%s http_server=%s url=http://%s/",
                isAp ? "AP" : "STA", iface != nullptr && net_if_is_up(iface) ? "up" : "down", ipStr,
                isAp ? "driver-managed (CONFIG_NXP_WIFI_SOFTAP_DHCP_SERVER)"
                     : "Zephyr DHCPv4 client (CONFIG_WIFI_STA_AUTO_DHCPV4)",
                httpServerStarted_ ? "listening" : "FAILED TO START",
                addr != nullptr ? ipStr : "<pending>");
    }

    static QcWifiWebTransport* instance_;

    k_thread thread_{};
    K_KERNEL_STACK_MEMBER(stack_, 2048);
    bool threadStarted_ = false;
    bool httpServerStarted_ = false;
    k_sem broadcastSem_{};

    k_mutex dataMutex_{};
    // Member storage, not stack - see CODING_STANDARDS.md's "big objects
    // never go on a thread's stack" rule and ADR 0008's own reasoning.
    // catalogSnapshot_ is the source of truth QcSession hands us
    // (publishCatalog()); catalogJson_ is its encoded form, re-derived on
    // qc_wifi_web (broadcast()) and also what GET /api/presets.json
    // serves (via its own responseJson_ copy - see handlePresetsGet());
    // broadcastJson_ is the worker thread's own send copy, so a slow send
    // never holds dataMutex_.
    QcCatalogSnapshot catalogSnapshot_{};
    bool catalogDirty_ = false;
    etl::vector<char, kJsonCapacity> catalogJson_;
    etl::vector<char, kJsonCapacity> broadcastJson_;
    etl::vector<char, kJsonCapacity> responseJson_;
    etl::vector<int, kMaxWsClients> wsSockets_;
    QcPresetChangeEvent lastChangeEvent_{};
    bool changePending_ = false;
    uint8_t wsRecvBuffer_[256]{};

    net_mgmt_event_callback netMgmtCb_{};
    wifi_connect_req_params apParams_{};
    wifi_connect_req_params staParams_{};
};

inline QcWifiWebTransport* QcWifiWebTransport::instance_ = nullptr;

namespace detail {

// Kept small and embedded on purpose (issue #71: "doesn't need to look
// like the Quad Cortex yet") - fetches the catalog once on load, then
// re-renders from either the fetch or any websocket push using the same
// JSON shape (QcCatalogJson.hpp), plus an optional "lastChange" field a
// websocket push may carry (see QcWifiWebTransport::appendLastChange()).
inline constexpr const char kIndexHtml[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>QC Remote (Wi-Fi test)</title>
<style>
body{font-family:sans-serif;margin:1rem;color:#222}
h3{margin:0.8rem 0 0.2rem}
ul{margin:0 0 0.5rem 1.2rem;padding:0}
#status{color:#666;margin-bottom:1rem}
</style></head>
<body>
<h1>QC Remote &ndash; Wi-Fi test</h1>
<div id="status">loading&hellip;</div>
<div id="catalog"></div>
<script>
function render(d){
  var el=document.getElementById('catalog');
  var html='';
  (d.folders||[]).forEach(function(f){
    html+='<h3>'+escapeHtml(f.name)+'</h3><ul>';
    (f.presets||[]).forEach(function(p){
      html+='<li>['+p.index+'] '+escapeHtml(p.name)+'</li>';
    });
    html+='</ul>';
  });
  el.innerHTML=html;
  var s='presets: '+d.presetCount+' (generation '+d.generation+')';
  if(d.incomplete){ s+=' - listing may be incomplete'; }
  if(d.lastChange){ s+=' - preset change (cmd '+d.lastChange.command+', #'+d.lastChange.count+')'; }
  document.getElementById('status').textContent=s;
}
function escapeHtml(s){
  return String(s).replace(/[&<>]/g,function(c){
    return c==='&'?'&amp;':c==='<'?'&lt;':'&gt;';
  });
}
fetch('/api/presets.json').then(function(r){return r.json();}).then(render);
var ws=new WebSocket('ws://'+location.host+'/ws');
ws.onmessage=function(ev){ render(JSON.parse(ev.data)); };
</script>
</body></html>
)HTML";

}  // namespace detail

inline http_resource_detail_static g_qcIndexResourceDetail{
    .common = {
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .type = HTTP_RESOURCE_TYPE_STATIC,
        .content_encoding = nullptr,
        .content_type = "text/html",
    },
    .static_data = detail::kIndexHtml,
    .static_data_len = sizeof(detail::kIndexHtml) - 1,
};

inline http_resource_detail_dynamic g_qcPresetsResourceDetail{
    .common = {
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .content_encoding = nullptr,
        .content_type = "application/json",
    },
    .cb = &QcWifiWebTransport::handlePresetsGet,
    .user_data = nullptr,
};

inline uint8_t g_qcWsDataBuffer[256];

inline http_resource_detail_websocket g_qcWsResourceDetail{
    .common = {
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .type = HTTP_RESOURCE_TYPE_WEBSOCKET,
        .content_encoding = nullptr,
        .content_type = nullptr,
    },
    .ws_sock = -1,
    .cb = &QcWifiWebTransport::handleWsSetup,
    .data_buffer = g_qcWsDataBuffer,
    .data_buffer_len = sizeof(g_qcWsDataBuffer),
    .user_data = nullptr,
};

// HTTP_SERVICE_DEFINE/HTTP_RESOURCE_DEFINE's per-service iterable-section
// start/end symbols (STRUCT_SECTION_ITERABLE_ALTERNATE, zephyr/sys/
// iterable_sections.h) are produced by GNU ld's automatic section-boundary
// symbols - plain, unmangled names, regardless of what namespace the
// defining translation unit used. Declared/defined inside namespace
// qcbridge with ordinary C++ linkage, the compiler looks for/emits a
// mangled qcbridge::_http_resource_desc_qcWebService_list_start instead
// of the plain name ld actually provides - confirmed as a real link
// failure ("undefined reference"), not a hypothetical one. extern "C"
// gives these specific declarations C linkage (their *lookup* still
// resolves normally through this namespace) so the names match what the
// linker generates.
extern "C" {
HTTP_SERVICE_DEFINE(qcWebService, nullptr, &g_qcWebPort, CONFIG_HTTP_SERVER_MAX_CLIENTS, 4, nullptr,
                    nullptr, nullptr);
HTTP_RESOURCE_DEFINE(qcIndexResource, qcWebService, "/", &g_qcIndexResourceDetail);
HTTP_RESOURCE_DEFINE(qcPresetsResource, qcWebService, "/api/presets.json", &g_qcPresetsResourceDetail);
HTTP_RESOURCE_DEFINE(qcWsResource, qcWebService, "/ws", &g_qcWsResourceDetail);
}

}  // namespace qcbridge
