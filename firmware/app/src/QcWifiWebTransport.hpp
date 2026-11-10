#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dhcpv4_server.h>
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
#include "QcLiveUpdateEnvelope.hpp"
#include "QcMessageTally.hpp"
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
// serving the real web UI (issue #88, ADR 0011: Lit, build-time gzipped
// assets - see web/ui/ and the generated header this file #includes below)
// and a WebSocket over Zephyr's HTTP server (zephyr/net/http/server.h), on
// top of a Wi-Fi interface brought up either as an access point or a
// station, per CONFIG_WIFI_USAGE_MODE_AP/STA (chosen at build time - see
// Kconfig/wifi_credentials.conf.example).
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

        startServer();

#if defined(CONFIG_WIFI_USAGE_MODE_AP)
        startAccessPoint();
#else
        startStation();
#endif

        logStartupSummary();
    }

    // The HTTP/WebSocket half of start(), with no Wi-Fi in it: Zephyr's
    // HTTP server plus the worker thread that pushes to connected
    // websocket clients. Separate from start() so tests/wifi_web_http can
    // drive the real resources over the loopback interface on a board with
    // no Wi-Fi driver at all - the page, the JSON resource and the
    // WebSocket upgrade are all reachable without one. Production code
    // calls start(), not this.
    void startServer() {
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

        if (!threadStarted_) {
            k_thread_create(&thread_, stack_, K_KERNEL_STACK_SIZEOF(stack_), &threadEntryTrampoline,
                            this, nullptr, nullptr, kThreadPriority, 0, K_NO_WAIT);
            k_thread_name_set(&thread_, "qc_wifi_web");
            threadStarted_ = true;
        }
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
        // Issue #75: QcSession's post-connect tally forwards every tracked
        // command it sees, including continuously-streaming ones like
        // GlobalTempo (QcMessageTally.hpp's own isPresetChangeCommand()
        // lists the ones that actually mean a preset's identity/content
        // changed). This page only ever shows a "preset change" label, so
        // anything else is simply not this transport's concern - without
        // this check, a GlobalTempo bump reaching here (as it does, once
        // its own throttling window is due) used to get pushed as a patch
        // and the page mislabeled it "preset change (cmd 33, ...)". A
        // future BLE transport registered alongside this one still gets
        // every event QcSession forwards; this filter is local to the web
        // page's own interpretation, not the interface's contract.
        if (!isPresetChangeCommand(event.command)) {
            return;
        }
        // broadcast() sends this as its own lean "patch" envelope
        // (QcLiveUpdateEnvelope.hpp) rather than resending the whole
        // catalog - a preset-change event alone doesn't mean the folder/
        // preset listing changed (that's publishCatalog()'s job, sent as a
        // "snapshot"), just that something about the loaded preset did.
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
            // Smoke test of 5068403 (issue #71): catalogJson_ used to be
            // re-encoded *only* by the worker thread's broadcast(), so
            // this response depended on that thread having already woken
            // since the last publishCatalog(). Encode here too if the
            // snapshot is newer than the JSON, so a GET can never serve a
            // stale (or the startup placeholder) listing just because of
            // thread ordering. Still off qc_heartbeat - this runs on
            // Zephyr's own single HTTP server thread, which is what PR
            // #74's review asked for.
            instance_->encodeIfDirtyLocked();
            instance_->responseJson_.assign(instance_->catalogJson_.begin(),
                                            instance_->catalogJson_.end());
        }
        response_ctx->body = reinterpret_cast<const uint8_t*>(instance_->responseJson_.data());
        response_ctx->body_len = instance_->responseJson_.size();
        response_ctx->final_chunk = true;
        // Issue #71's smoke test could not tell "the browser never asked"
        // from "the firmware answered and the browser rejected it" - one
        // line per request settles that, and the page's own status line
        // reports its side (see web/ui/src/store.js's own
        // #fetchInitialCatalog(), issue #88).
        LOG_INF("Served GET /api/presets.json (%zu bytes)", instance_->responseJson_.size());
        return 0;
    }

    static int handleWsSetup(int wsSocket, http_request_ctx* /*request_ctx*/, void* /*user_data*/) {
        if (instance_ != nullptr) {
            instance_->addWsClient(wsSocket);
            LOG_INF("Websocket client connected (sock=%d)", wsSocket);
            // Smoke test of 5068403 (issue #71): a newly connected client
            // used to receive nothing at all until the *next* publish.
            // publishCatalog() only fires when a File stream finishes
            // (once per device connect) and publishPresetChange() is
            // throttled to one log/push per minute per command, so a page
            // opened after the catalog was already published sat empty for
            // up to a minute - which is what "the page shows only the
            // title and Loading..." looked like. Wake the worker so it
            // sends a fresh "snapshot" (QcLiveUpdateEnvelope.hpp) to this
            // client right away, with a seq of its own starting from 0.
            instance_->requestFreshSnapshot();
        }
        return 0;
    }

private:
    static constexpr int kThreadPriority = 10;
    static constexpr size_t kJsonCapacity = 16384;
    // Issue #75 / ADR 0011: Zephyr's HTTP server hands this resource's one
    // static data_buffer (g_qcWsDataBuffer, below) to websocket_register()
    // on *every* upgrade (subsys/net/lib/http/http_server_ws.c:126-128 in
    // the pinned v4.5.0 tree - the buffer comes from the resource's own
    // http_resource_detail_websocket, not from anything per-connection),
    // which only stores the pointer per-context
    // (subsys/net/lib/websocket/websocket.c:1330) - so two concurrently
    // upgraded clients on this one URL really would share one receive
    // buffer and could corrupt each other's partially received frames.
    // ADR 0010 first found this and left it for this issue; this issue
    // adds a client-to-server message (resync), so "the page never sends"
    // is no longer true either. The clean fix within Zephyr's API - a
    // second static resource/buffer pair for a second URL - would still
    // need the page to know which URL is free, for a feature (two
    // simultaneous live clients) nothing has asked for. Capping this at
    // one live client is simpler and provably safe instead:
    // CONFIG_WEBSOCKET_MAX_CONTEXTS=1 (prj.conf) means Zephyr itself
    // refuses a second concurrent upgrade - websocket_get() finds no free
    // context and websocket_register() returns -ENOENT - before this
    // class's own code ever runs, so there is structurally never more
    // than one context sharing the buffer. See firmware/README.md for
    // what a second browser sees under this policy.
    static constexpr size_t kMaxWsClients = 1;
    // How often run() wakes even without a publish, to poll connected
    // sockets for control frames/EOF (PR #74 review: the worker only ever
    // woke on a broadcast, so a closed browser tab's socket was never
    // drained or unregistered until the next unrelated publish happened to
    // fail a send to it) and to check whether a tick heartbeat is due.
    static constexpr int kPollIntervalMs = 1000;
    // Issue #75 / ADR 0011: without a heartbeat, an idle rig (nothing
    // published) and a half-open socket after a Wi-Fi drop look identical
    // to the page - silence either way. Chosen to match
    // web/poc/src/store.js's own kTickIntervalMs (same ADR), so a future
    // move to that reference client doesn't also have to change this
    // number. Kconfig, not a bare constant, per CODING_STANDARDS.md
    // ("feature toggles... go through Kconfig").
    static constexpr int64_t kTickIntervalMs = CONFIG_QC_WIFI_WEB_TICK_INTERVAL_MS;
    // Envelope overhead (QcLiveUpdateEnvelope.hpp) on top of the bare
    // catalog JSON a "snapshot" wraps - {"v":1,"t":"snapshot","seq":<=10
    // digits>,"catalog": plus a trailing "}". 64 leaves comfortable slack
    // over the ~42 bytes that actually costs.
    static constexpr size_t kEnvelopeOverhead = 64;

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
            maybeSendTick();
        }
    }

    // Decides what this wake sends - a full "snapshot" (a new client just
    // connected, a resync asked for one, or the catalog itself changed) or
    // a lean "patch" (only a preset-change event arrived, catalog
    // unchanged) - encodes it into broadcastJson_ and hands off to
    // sendEnvelopeToClients() for the actual socket I/O. See
    // QcLiveUpdateEnvelope.hpp for both shapes.
    void broadcast() {
        enum class Kind { None, Snapshot, Patch };
        Kind kind = Kind::None;
        QcPresetChangeEvent change{};

        {
            ScopedKMutex lock(dataMutex_);
            const bool wantSnapshot = pendingInitialSnapshot_ || catalogDirty_;
            // Moved here from publishCatalog() per PR #74 review, so the
            // variable-length formatting work never runs on qc_heartbeat.
            encodeIfDirtyLocked();

            if (wantSnapshot) {
                kind = Kind::Snapshot;
                pendingInitialSnapshot_ = false;
                // A snapshot already reports the full, current state, so a
                // patch describing an older preset-change event right
                // behind it would be redundant at best, stale at worst.
                changePending_ = false;
            } else if (changePending_) {
                kind = Kind::Patch;
                change = lastChangeEvent_;
                changePending_ = false;
            }

            if (kind == Kind::None) {
                return;  // Nothing actually pending - shouldn't normally
                         // happen (run() only calls broadcast() when the
                         // semaphore fired), harmless if it does.
            }

            if (kind == Kind::Snapshot) {
                encodeSnapshotEnvelope(wsSeq_, catalogJson_, broadcastJson_);
            } else {
                encodePatchEnvelope(wsSeq_, change, broadcastJson_);
            }
        }

        sendEnvelopeToClients(kind == Kind::Snapshot ? "snapshot" : "patch");
    }

    // A tick carries no payload beyond the envelope itself - just evidence
    // the board is still there. Sent only once kTickIntervalMs has passed
    // with nothing else sent to this client, so an active session's own
    // snapshots/patches already satisfy the same liveness purpose without
    // a redundant tick right next to them.
    void maybeSendTick() {
        etl::vector<int, kMaxWsClients> sockets;
        {
            ScopedKMutex lock(dataMutex_);
            sockets = wsSockets_;
        }
        if (sockets.empty()) {
            return;
        }
        if (k_uptime_get() - lastSendUptimeMs_ < kTickIntervalMs) {
            return;
        }

        {
            ScopedKMutex lock(dataMutex_);
            encodeTickEnvelope(wsSeq_, broadcastJson_);
        }
        sendEnvelopeToClients("tick");
    }

    // Sends whatever's currently in broadcastJson_ to every connected
    // client (at most kMaxWsClients worth), bumping the shared seq counter
    // and the liveness clock once per message - never holding dataMutex_
    // across the actual socket sends (same split PR #74 review asked for:
    // a wedged client must only ever delay this thread's own next send,
    // never block it forever).
    void sendEnvelopeToClients(const char* label) {
        etl::vector<int, kMaxWsClients> sockets;
        size_t length = 0;
        {
            ScopedKMutex lock(dataMutex_);
            sockets = wsSockets_;
            length = broadcastJson_.size();
            ++wsSeq_;
            lastSendUptimeMs_ = k_uptime_get();
        }

        for (const int sock : sockets) {
            const int sent = websocket_send_msg(sock, reinterpret_cast<const uint8_t*>(broadcastJson_.data()),
                                                length, WEBSOCKET_OPCODE_DATA_TEXT,
                                                /*mask=*/false, /*final=*/true, 2000);
            if (sent < 0) {
                LOG_INF("Websocket client %d gone (err=%d), dropping", sock, sent);
                removeWsClient(sock);
                (void)websocket_unregister(sock);
            } else {
                // Same reason handlePresetsGet() logs its own response:
                // the 5068403 smoke test had no way to tell a push that
                // never happened from one the page failed to render.
                LOG_INF("Pushed %s to websocket client %d (%zu bytes)", label, sock, length);
            }
        }
    }

    // PR #74 review: this worker previously only ever woke to broadcast,
    // so Zephyr handed it an upgraded socket (handleWsSetup()) that was
    // then never read from again - a closed browser tab left its slot
    // occupied until an unrelated broadcast happened to fail a send to
    // it. Runs every run() wake (see kPollIntervalMs), draining any
    // pending frame per connected socket without blocking, replying to
    // pings, unregistering anything closed or erroring, and - issue #75 -
    // recognizing the one inbound message this firmware understands.
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
            } else if (messageType & WEBSOCKET_FLAG_TEXT) {
                // Issue #75: the only inbound message this firmware
                // recognizes - QcLiveUpdateEnvelope.hpp's
                // isResyncMessage(). Anything else this doesn't recognize
                // (or a frame fragment with no complete message in it) is
                // simply ignored, same as before.
                const std::string_view text(reinterpret_cast<const char*>(wsRecvBuffer_),
                                            static_cast<size_t>(received));
                if (isResyncMessage(text)) {
                    LOG_INF("Websocket client %d asked for a resync", sock);
                    requestFreshSnapshot();
                }
            }
        }
    }

    // Re-encodes catalogJson_ from the latest published snapshot if one
    // arrived since the last encode. Call with dataMutex_ held - both
    // callers (broadcast() on qc_wifi_web, handlePresetsGet() on Zephyr's
    // HTTP server thread) already hold it.
    void encodeIfDirtyLocked() {
        if (catalogDirty_) {
            encodeCatalogJson(catalogSnapshot_, catalogJson_);
            catalogDirty_ = false;
        }
    }

    // Forces the next broadcast() to send a full "snapshot" rather than
    // whatever it would otherwise send, and restarts this connection's seq
    // counter at 0 - used both when a client just connected
    // (handleWsSetup()'s own comment) and when one asks for a resync after
    // noticing a seq gap (pollWsClients()). Either way the client is about
    // to get a fresh, complete baseline, so a fresh seq to count from is
    // exactly as valid as continuing the old one, and simpler to test.
    void requestFreshSnapshot() {
        {
            ScopedKMutex lock(dataMutex_);
            wsSeq_ = 0;
            pendingInitialSnapshot_ = true;
        }
        k_sem_give(&broadcastSem_);
    }

    void addWsClient(int sock) {
        ScopedKMutex lock(dataMutex_);
        if (wsSockets_.full()) {
            // Issue #75 / ADR 0011: shouldn't normally be reachable - see
            // kMaxWsClients' own comment. Zephyr refuses a second
            // concurrent upgrade before this callback ever runs; this is
            // a defensive belt-and-braces check, not the actual
            // enforcement.
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
    // Kconfig). ADR 0010's original claim that the driver never does this
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
    // (boards/frdm_rw612.conf), and the driver brings up the static IP and
    // DHCPv4 server itself once WLAN_REASON_UAP_SUCCESS fires.
    //
    // Other drivers don't (the ESP32's doesn't - Zephyr's apsta_mode sample
    // starts its own server for it). For those, CONFIG_QC_WIFI_AP_APP_DHCP_SERVER
    // (app Kconfig, on whenever the driver isn't NXP's) has this class set the
    // address and start the server itself, before enabling the AP, the same
    // way that sample does - see startAppDhcpServer().
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

#if defined(CONFIG_QC_WIFI_AP_APP_DHCP_SERVER)
        if (!startAppDhcpServer(iface)) {
            return;
        }
        LOG_INF("Wi-Fi AP \"%s\" starting (DHCPv4 server started by this firmware - "
                "CONFIG_QC_WIFI_AP_IP_ADDRESS=%s)",
                CONFIG_QC_WIFI_AP_SSID, CONFIG_QC_WIFI_AP_IP_ADDRESS);
#else
        LOG_INF("Wi-Fi AP \"%s\" starting (DHCPv4 server is driver-managed - "
                "CONFIG_NXP_WIFI_SOFTAP_IP_ADDRESS=%s)",
                CONFIG_QC_WIFI_AP_SSID, CONFIG_NXP_WIFI_SOFTAP_IP_ADDRESS);
#endif
        const int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &apParams_, sizeof(apParams_));
        if (ret != 0) {
            LOG_ERR("NET_REQUEST_WIFI_AP_ENABLE failed: %d", ret);
        }
#endif
    }

#if defined(CONFIG_QC_WIFI_AP_APP_DHCP_SERVER)
    // Static address, netmask and gateway on the AP interface, then a DHCPv4
    // server whose pool starts 10 addresses above it - the same steps as
    // Zephyr's samples/net/wifi/apsta_mode enable_dhcpv4_server().
    static bool startAppDhcpServer(net_if* iface) {
        net_in_addr addr{};
        net_in_addr netmask{};
        if (net_addr_pton(NET_AF_INET, CONFIG_QC_WIFI_AP_IP_ADDRESS, &addr) != 0 ||
            net_addr_pton(NET_AF_INET, CONFIG_QC_WIFI_AP_NETMASK, &netmask) != 0) {
            LOG_ERR("Invalid CONFIG_QC_WIFI_AP_IP_ADDRESS/_NETMASK");
            return false;
        }

        net_if_ipv4_set_gw(iface, &addr);
        if (net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0) == nullptr) {
            LOG_ERR("Unable to set the AP interface's IPv4 address");
            return false;
        }
        if (!net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask)) {
            LOG_ERR("Unable to set the AP interface's netmask");
            return false;
        }

        net_in_addr poolStart = addr;
        poolStart.s4_addr[3] += 10;
        const int ret = net_dhcpv4_server_start(iface, &poolStart);
        if (ret != 0) {
            LOG_ERR("DHCPv4 server start failed: %d", ret);
            return false;
        }
        return true;
    }
#endif

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
        // No band hint here, deliberately (ADR 0010 originally hardcoded
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
    // the event's own status field.
    //
    // Hardware finding (5068403 smoke test, issue #71): that same
    // WIFI_STATUS_CONN_WRONG_PASSWORD then appeared ~52s into a session
    // on a correct passphrase. Traced through the vendor stack: the NXP
    // Wi-Fi middleware's connection manager reports an asynchronous
    // disassociation from the AP by calling
    // do_connect_failed(WLAN_REASON_NETWORK_AUTH_FAILED) *unconditionally*
    // - wlcm_process_disassoc_event() in modules/hal/nxp/mcux/middleware/
    // wifi_nxp/wlcmgr/wlan.c, which ignores the 802.11 reason code
    // entirely - and the link-loss path does the same for every reason
    // other than a 4-way-handshake timeout. Zephyr's driver then maps that
    // one WLAN_REASON value to "Wrong password". So the status is not a
    // credential diagnosis at all: it covers an AP-initiated
    // disassoc/deauth, a lost EAPoL frame, and an actually-wrong
    // passphrase alike. wlan.reassoc_control then re-associates on its
    // own, which is the "Wi-Fi station connected" line that follows.
    //
    // status_code/reason_code are deliberately no longer printed on this
    // path: wifi_mgmt_raise_connect_result_event() (subsys/net/l2/wifi/
    // wifi_mgmt.c) fills in only .status and leaves both at 0, and this
    // driver never uses the _status_event() variant that could set them -
    // so "status_code=0, reason_code=0" was structurally guaranteed and
    // read as "no information available", which it was.
    void onWifiEvent(uint64_t mgmtEvent, net_if* iface, net_mgmt_event_callback* cb) {
        const auto* status = static_cast<const wifi_status*>(cb->info);
        switch (mgmtEvent) {
        case NET_EVENT_WIFI_CONNECT_RESULT:
            if (status != nullptr && status->conn_status == WIFI_STATUS_CONN_SUCCESS) {
                LOG_INF("Wi-Fi station connected: %s", wifi_conn_status_txt(status->conn_status));
                staConnectedOnce_ = true;
                logReadySummary(iface, /*isAp=*/false);
            } else if (staConnectedOnce_) {
                // Smoke test of 5068403 (issue #71): this is the line that
                // read "Wi-Fi station connect failed: Wrong password" ~52s
                // into a working session, on a correct passphrase. The
                // status is accurate as reported and wrong as a diagnosis,
                // and the misreport is in the vendor stack, not here - see
                // this handler's own note above for the trace.
                LOG_WRN("Wi-Fi station link dropped and is re-associating - the NXP stack "
                        "reported it as \"%s\", which it also reports for an AP-initiated "
                        "disassociation/deauthentication, so it is NOT evidence of a wrong "
                        "passphrase. Expect a \"Wi-Fi station connected\" line next.",
                        status != nullptr ? wifi_conn_status_txt(status->conn_status) : "unknown");
            } else {
                LOG_WRN("Wi-Fi station connect failed: %s - no association has succeeded yet, "
                        "so check CONFIG_QC_WIFI_STA_SSID/CONFIG_QC_WIFI_STA_PSK first",
                        status != nullptr ? wifi_conn_status_txt(status->conn_status) : "unknown");
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
                isAp ? kApDhcpDescription : "Zephyr DHCPv4 client (CONFIG_WIFI_STA_AUTO_DHCPV4)",
                httpServerStarted_ ? "listening" : "FAILED TO START",
                addr != nullptr ? ipStr : "<pending>");
    }

#if defined(CONFIG_QC_WIFI_AP_APP_DHCP_SERVER)
    static constexpr const char* kApDhcpDescription =
        "started by this firmware (CONFIG_QC_WIFI_AP_APP_DHCP_SERVER)";
#else
    static constexpr const char* kApDhcpDescription =
        "driver-managed (CONFIG_NXP_WIFI_SOFTAP_DHCP_SERVER)";
#endif

    static QcWifiWebTransport* instance_;

    k_thread thread_{};
    K_KERNEL_STACK_MEMBER(stack_, 2048);
    bool threadStarted_ = false;
    bool httpServerStarted_ = false;
    // Set once a station association has actually succeeded, so a later
    // failure event can be logged as the re-association it is rather than
    // as a credential problem - see onWifiEvent().
    bool staConnectedOnce_ = false;
    k_sem broadcastSem_{};

    k_mutex dataMutex_{};
    // Member storage, not stack - see CODING_STANDARDS.md's "big objects
    // never go on a thread's stack" rule and ADR 0008's own reasoning.
    // catalogSnapshot_ is the source of truth QcSession hands us
    // (publishCatalog()); catalogJson_ is its encoded, *bare* form,
    // re-derived on qc_wifi_web (broadcast()) and also what GET
    // /api/presets.json serves (via its own responseJson_ copy - see
    // handlePresetsGet()) - neither of those carries the WebSocket
    // envelope (QcLiveUpdateEnvelope.hpp), since the HTTP endpoint's
    // contract predates issue #75 and doesn't need it. broadcastJson_ is
    // the worker thread's own send copy - enveloped, so sized
    // kEnvelopeOverhead bytes larger than the other two - so a slow send
    // never holds dataMutex_.
    QcCatalogSnapshot catalogSnapshot_{};
    bool catalogDirty_ = false;
    etl::vector<char, kJsonCapacity> catalogJson_;
    etl::vector<char, kJsonCapacity + kEnvelopeOverhead> broadcastJson_;
    etl::vector<char, kJsonCapacity> responseJson_;
    etl::vector<int, kMaxWsClients> wsSockets_;
    QcPresetChangeEvent lastChangeEvent_{};
    bool changePending_ = false;
    // Issue #75: forces the next broadcast() to send a full "snapshot" -
    // see requestFreshSnapshot().
    bool pendingInitialSnapshot_ = false;
    // This connection's own seq counter (QcLiveUpdateEnvelope.hpp) and the
    // uptime (k_uptime_get()) of the last message actually sent to it -
    // together, what maybeSendTick() needs to decide a tick is due.
    uint32_t wsSeq_ = 0;
    int64_t lastSendUptimeMs_ = 0;
    uint8_t wsRecvBuffer_[256]{};

    net_mgmt_event_callback netMgmtCb_{};
    wifi_connect_req_params apParams_{};
    wifi_connect_req_params staParams_{};
};

inline QcWifiWebTransport* QcWifiWebTransport::instance_ = nullptr;

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

// Issue #75 / ADR 0011: this one static buffer is what CONFIG_WEBSOCKET_
// MAX_CONTEXTS=1 (prj.conf) makes safe to share - see kMaxWsClients' own
// comment for the full mechanism. Zephyr hands this exact pointer to
// websocket_register() on every upgrade
// (subsys/net/lib/http/http_server_ws.c:126-128, pinned v4.5.0 tree), and
// with at most one context ever in use, there is never a second
// concurrent user of it to corrupt.
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
HTTP_RESOURCE_DEFINE(qcPresetsResource, qcWebService, "/api/presets.json", &g_qcPresetsResourceDetail);
HTTP_RESOURCE_DEFINE(qcWsResource, qcWebService, "/ws", &g_qcWsResourceDetail);
}

// Issue #88 / ADR 0011: the real web UI (web/ui/), embedded at build time
// by firmware/app/tools/embed_assets.py (see
// firmware/app/cmake/EmbedWebAssets.cmake) as gzipped static resources on
// this same qcWebService - including "/", which used to be this file's own
// hand-written kIndexHtml. Generated, not committed; depends on
// qcWebService (just above) and g_qcWebPort/BIT/HTTP_GET (already in scope
// from this file's own includes).
#include "qc_web_assets_generated.hpp"

}  // namespace qcbridge
