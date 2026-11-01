#pragma once

#include <algorithm>
#include <cstdint>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/websocket.h>
#include <zephyr/net/wifi_mgmt.h>

#include <etl/vector.h>

#include "QcCatalogJson.hpp"
#include "QcCatalogSnapshot.hpp"
#include "QcPresetTransport.hpp"
#include "ScopedKMutex.hpp"

LOG_MODULE_REGISTER(qc_wifi_web, LOG_LEVEL_INF);

namespace qcbridge {

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
    }

    // Brings up Wi-Fi and starts the worker thread that pushes updates to
    // connected websocket clients. Call once at startup (Main.cpp), before
    // QcSession::addTransport(*this).
    void start() {
        net_mgmt_init_event_callback(&netMgmtCb_, &onWifiEventTrampoline, kNetEventMask);
        net_mgmt_add_event_callback(&netMgmtCb_);

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
    }

    // QcPresetTransport overrides - called from QcSession's own heartbeat/
    // dispatcher threads (see that header's contract). Both just update
    // member storage and signal the worker thread; the actual socket I/O
    // (which can block on a slow/absent Wi-Fi link) only ever happens on
    // qc_wifi_web, never here - so a slow client can never delay the
    // ~200ms KeepAlive.
    void publishCatalog(const QcCatalogSnapshot& snapshot) override {
        {
            ScopedKMutex lock(dataMutex_);
            encodeCatalogJson(snapshot, catalogJson_);
        }
        k_sem_give(&broadcastSem_);
    }

    void publishPresetChange(const QcPresetChangeEvent& /*event*/) override {
        // The client's own job is to re-render from the latest catalog
        // JSON (QcPresetTransport.hpp's own comment on why this event
        // carries no protocol detail) - rebroadcasting the same stored
        // catalogJson_ again covers "a preset changed but no full
        // re-listing happened" with the one message shape the page
        // already understands, rather than inventing a second one.
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
        ScopedKMutex lock(instance_->dataMutex_);
        response_ctx->body = instance_->catalogJson_.data();
        response_ctx->body_len = instance_->catalogJson_.size();
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

    static constexpr uint64_t kNetEventMask =
        (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT |
         NET_EVENT_WIFI_AP_ENABLE_RESULT | NET_EVENT_WIFI_AP_DISABLE_RESULT |
         NET_EVENT_WIFI_AP_STA_CONNECTED | NET_EVENT_WIFI_AP_STA_DISCONNECTED);

    static void threadEntryTrampoline(void* p1, void*, void*) {
        static_cast<QcWifiWebTransport*>(p1)->run();
    }

    void run() {
        while (true) {
            k_sem_take(&broadcastSem_, K_FOREVER);
            broadcast();
        }
    }

    // Copies the latest catalog JSON out under dataMutex_ (a plain
    // byte-copy, never blocking) before sending it to every connected
    // client - same split as QcPresetPrinter::printCatalog()'s own
    // copy-then-print, for the same reason: publishCatalog() above must
    // never wait on a slow socket send.
    void broadcast() {
        size_t length = 0;
        etl::vector<int, kMaxWsClients> sockets;
        {
            ScopedKMutex lock(dataMutex_);
            length = catalogJson_.size();
            broadcastJson_.assign(catalogJson_.begin(), catalogJson_.end());
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
        apParams_.band = WIFI_FREQ_BAND_2_4_GHZ;
        if (sizeof(CONFIG_QC_WIFI_AP_PSK) > 1) {
            apParams_.psk = kPsk;
            apParams_.psk_length = sizeof(CONFIG_QC_WIFI_AP_PSK) - 1;
            apParams_.security = WIFI_SECURITY_TYPE_PSK;
        } else {
            LOG_WRN("CONFIG_QC_WIFI_AP_PSK is empty - starting an OPEN access point. Owner's "
                    "decision (issue #71) is to use a WPA2 passphrase - set CONFIG_QC_WIFI_AP_PSK "
                    "in a local wifi_credentials.conf (see wifi_credentials.conf.example).");
            apParams_.security = WIFI_SECURITY_TYPE_NONE;
        }

        const int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &apParams_, sizeof(apParams_));
        if (ret != 0) {
            LOG_ERR("NET_REQUEST_WIFI_AP_ENABLE failed: %d", ret);
        } else {
            LOG_INF("Wi-Fi AP \"%s\" starting", CONFIG_QC_WIFI_AP_SSID);
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
        staParams_.band = WIFI_FREQ_BAND_2_4_GHZ;
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

    void onWifiEvent(uint64_t mgmtEvent, net_if* /*iface*/, net_mgmt_event_callback* cb) {
        switch (mgmtEvent) {
        case NET_EVENT_WIFI_CONNECT_RESULT:
            LOG_INF("Wi-Fi station connected");
            break;
        case NET_EVENT_WIFI_DISCONNECT_RESULT:
            LOG_INF("Wi-Fi station disconnected");
            break;
        case NET_EVENT_WIFI_AP_ENABLE_RESULT:
            LOG_INF("Wi-Fi AP enabled - waiting for a station to join");
            break;
        case NET_EVENT_WIFI_AP_DISABLE_RESULT:
            LOG_INF("Wi-Fi AP disabled");
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

    static QcWifiWebTransport* instance_;

    k_thread thread_{};
    K_KERNEL_STACK_MEMBER(stack_, 2048);
    bool threadStarted_ = false;
    k_sem broadcastSem_{};

    k_mutex dataMutex_{};
    // Member storage, not stack - see CODING_STANDARDS.md's "big objects
    // never go on a thread's stack" rule and ADR 0008's own reasoning.
    // catalogJson_ is also what GET /api/presets.json serves directly;
    // broadcastJson_ is the worker thread's own copy, so a slow send never
    // holds dataMutex_ (see broadcast()'s own comment).
    etl::vector<char, kJsonCapacity> catalogJson_;
    etl::vector<char, kJsonCapacity> broadcastJson_;
    etl::vector<int, kMaxWsClients> wsSockets_;

    net_mgmt_event_callback netMgmtCb_{};
    wifi_connect_req_params apParams_{};
    wifi_connect_req_params staParams_{};
};

inline QcWifiWebTransport* QcWifiWebTransport::instance_ = nullptr;

namespace detail {

// Kept small and embedded on purpose (issue #71: "doesn't need to look
// like the Quad Cortex yet") - fetches the catalog once on load, then
// re-renders from either the fetch or any websocket push using the same
// JSON shape (QcCatalogJson.hpp).
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

inline uint16_t g_qcWebPort = 80;

HTTP_SERVICE_DEFINE(qcWebService, nullptr, &g_qcWebPort, CONFIG_HTTP_SERVER_MAX_CLIENTS, 4, nullptr,
                    nullptr, nullptr);
HTTP_RESOURCE_DEFINE(qcIndexResource, qcWebService, "/", &g_qcIndexResourceDetail);
HTTP_RESOURCE_DEFINE(qcPresetsResource, qcWebService, "/api/presets.json", &g_qcPresetsResourceDetail);
HTTP_RESOURCE_DEFINE(qcWsResource, qcWebService, "/ws", &g_qcWsResourceDetail);

}  // namespace qcbridge
