// Issue #71: drives QcWifiWebTransport.hpp's real HTTP/WebSocket resources
// over a real TCP socket on Zephyr's loopback interface, doing what a
// browser actually does: GET / and then GET /api/presets.json (both on one
// keep-alive connection and on a fresh one), plus a WebSocket upgrade.
// Issue #88 / ADR 0011 added coverage for the embed step's generated
// static resources (the real web UI, pre-gzipped) alongside these.
//
// This exists because the 5068403 smoke test's "the page shows only the
// title and Loading..." could not be reproduced or ruled out anywhere -
// the page's own request sequence had never been exercised off-board, so
// every explanation was a reading of Zephyr's server rather than a result.
// Nothing here needs Wi-Fi, a Wi-Fi driver, or a board: the transport's
// startServer() brings up only the HTTP/WebSocket half.
//
// The CONFIG_QC_WIFI_STA_* defines below are what the header's station
// path (compiled, never called here - this suite never calls start())
// needs to compile. They are test-only; the real values come from a local
// wifi_credentials.conf in a firmware build.
#define CONFIG_QC_WIFI_STA_SSID ""
#define CONFIG_QC_WIFI_STA_PSK ""
// Issue #75: this test app has no Kconfig of its own (unlike the real
// firmware/app build, see ../../Kconfig), so QcWifiWebTransport.hpp's own
// CONFIG_QC_WIFI_WEB_TICK_INTERVAL_MS reference needs a value the same way
// the two above do. Shorter than the real 5000ms default, so
// test_tick_is_sent_when_nothing_else_is doesn't have to wait that long -
// but deliberately kept *above* receiveAll()'s own kRecvTimeoutSeconds
// (2000ms): every other test in this suite reads a WebSocket response
// with receiveAll(), which decides a response is complete once the socket
// goes quiet for kRecvTimeoutSeconds - and once a client is connected,
// this transport never *stops* being busy again on its own (that's the
// heartbeat's whole point). Keeping the tick slower than that read timeout
// is what lets those other tests still see a clean "quiet" after the one
// message they're reading, instead of hanging until this suite's own
// overall timeout.
#define CONFIG_QC_WIFI_WEB_TICK_INTERVAL_MS 2500

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
// QcWifiWebTransport.hpp logs through Zephyr's LOG_* macros, which need a
// registered module in whichever translation unit includes it (Main.cpp
// registers "main").
LOG_MODULE_REGISTER(wifi_web_http_test, LOG_LEVEL_INF);

#include <zephyr/net/http/service.h>
#include <zephyr/net/socket.h>
#include <zephyr/ztest.h>

#include "QcGzip.hpp"
#include "QcPresetCatalog.hpp"
#include "QcWifiWebTransport.hpp"

using namespace qcbridge;

namespace {

constexpr const char* kServerAddress = "127.0.0.1";
constexpr uint16_t kServerPort = 8080;
constexpr int kRecvTimeoutSeconds = 2;

// Real request headers, as Safari/Chrome send them for a top-level
// navigation and for a fetch() from the page's own script - deliberately
// not a minimal hand-written request, since the point is to exercise the
// server with what the browser in the smoke test actually sent (header
// count and sizes included: CONFIG_HTTP_SERVER_CLIENT_BUFFER_SIZE is 256,
// so these span several reads).
constexpr const char kNavigationRequest[] =
    "GET / HTTP/1.1\r\n"
    "Host: 192.168.1.233\r\n"
    "Upgrade-Insecure-Requests: 1\r\n"
    "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
    "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/18.0 Safari/605.1.15\r\n"
    "Accept-Language: en-GB,en;q=0.9\r\n"
    "Accept-Encoding: gzip, deflate\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

constexpr const char kFetchRequest[] =
    "GET /api/presets.json HTTP/1.1\r\n"
    "Host: 192.168.1.233\r\n"
    "Accept: */*\r\n"
    "Referer: http://192.168.1.233/\r\n"
    "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/18.0 Safari/605.1.15\r\n"
    "Accept-Language: en-GB,en;q=0.9\r\n"
    "Accept-Encoding: gzip, deflate\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

// Issue #88 / ADR 0011: same shape as kFetchRequest, for one of the real
// web UI's own vendored assets - confirms the embed step's generated
// resources (firmware/app/tools/embed_assets.py) are reachable by path,
// not just present in the generated header's own table.
constexpr const char kVendorLitRequest[] =
    "GET /vendor/lit.js HTTP/1.1\r\n"
    "Host: 192.168.1.233\r\n"
    "Accept: */*\r\n"
    "Referer: http://192.168.1.233/\r\n"
    "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/18.0 Safari/605.1.15\r\n"
    "Accept-Language: en-GB,en;q=0.9\r\n"
    "Accept-Encoding: gzip, deflate\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

// Sec-WebSocket-Key is any 16 random bytes, base64-encoded; the value
// below is the one RFC 6455 section 1.3 uses in its own example.
constexpr const char kWebsocketRequest[] =
    "GET /ws HTTP/1.1\r\n"
    "Host: 192.168.1.233\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Origin: http://192.168.1.233\r\n"
    "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/18.0 Safari/605.1.15\r\n"
    "\r\n";

// The transport and the catalog are several KB each - static storage, not
// a test-function local (same rule as tests/preset_transport's own
// comment, and the same fault that taught it).
QcWifiWebTransport& transport() {
    static QcWifiWebTransport instance;
    return instance;
}

QcPresetCatalog& catalog() {
    static QcPresetCatalog instance;
    return instance;
}

QcCatalogSnapshot& snapshot() {
    static QcCatalogSnapshot instance;
    return instance;
}

char g_responseBuffer[8192];

// The catalog from the 5068403 smoke test's own log: 3 folders,
// 22 + 2 + 25 = 49 presets.
void publishSmokeTestCatalog(bool incomplete = false) {
    catalog().reset();
    const struct {
        const char* key;
        const char* name;
        int count;
    } kFolders[] = {{"my", "My Presets", 22}, {"practice", "Practice", 2}, {"dl", "Downloads", 25}};

    uint32_t index = 0;
    for (const auto& folder : kFolders) {
        for (int i = 0; i < folder.count; ++i) {
            char presetName[32];
            (void)snprintf(presetName, sizeof(presetName), "%s preset %d", folder.name, i);
            catalog().addPreset(FilePreset{.folderKey = folder.key,
                                           .folderName = folder.name,
                                           .presetName = presetName,
                                           .index = index});
            ++index;
        }
    }
    copyCatalogSnapshot(catalog(), incomplete, snapshot());
    transport().publishCatalog(snapshot());
}

int connectToServer() {
    const int sock = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);
    zassert_true(sock >= 0, "socket() failed (%d)", errno);

    const timeval timeout{.tv_sec = kRecvTimeoutSeconds, .tv_usec = 0};
    zassert_ok(zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &timeout,
                                sizeof(timeout)),
               "setsockopt() failed (%d)", errno);

    net_sockaddr_in address{};
    address.sin_family = NET_AF_INET;
    address.sin_port = net_htons(kServerPort);
    zassert_equal(zsock_inet_pton(NET_AF_INET, kServerAddress, &address.sin_addr.s_addr), 1,
                  "inet_pton() failed");
    zassert_ok(zsock_connect(sock, reinterpret_cast<net_sockaddr*>(&address), sizeof(address)),
               "connect() failed (%d)", errno);
    return sock;
}

void sendRequest(int sock, const char* request) {
    const size_t length = strlen(request);
    const ssize_t sent = zsock_send(sock, request, length, 0);
    zassert_equal(sent, static_cast<ssize_t>(length), "send() sent %zd of %zu (%d)", sent, length,
                  errno);
}

// Reads until the socket goes quiet (recv times out) or the buffer is
// full, and returns what arrived as a NUL-terminated string. The
// responses under test are a few KB at most, and reading to quiet rather
// than parsing Content-Length/chunk lengths is what lets a *malformed*
// response be asserted against too - a response that never arrives, or
// arrives without a status line, is exactly the failure being chased.
std::string_view receiveAll(int sock) {
    size_t offset = 0;
    while (offset + 1 < sizeof(g_responseBuffer)) {
        const ssize_t received =
            zsock_recv(sock, g_responseBuffer + offset, sizeof(g_responseBuffer) - 1 - offset, 0);
        if (received <= 0) {
            break;
        }
        offset += static_cast<size_t>(received);
    }
    g_responseBuffer[offset] = '\0';
    return std::string_view(g_responseBuffer, offset);
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// Issue #88 / ADR 0011: a static resource's response has no chunked
// framing (unlike the dynamic /api/presets.json one tested above) - just
// status line, headers, then the body back to back - so the body is
// everything after the blank line that ends the headers (RFC 9112
// section 2.1). receiveAll() already captured any embedded NUL bytes the
// gzip body contains correctly (it bounds by byte count, not strlen), and
// this just slices off the HTTP framing in front of them.
std::string_view bodyOf(std::string_view httpResponse) {
    constexpr std::string_view kHeaderEnd = "\r\n\r\n";
    const size_t pos = httpResponse.find(kHeaderEnd);
    zassert_true(pos != std::string_view::npos, "no end-of-headers marker in response");
    return httpResponse.substr(pos + kHeaderEnd.size());
}

// Scratch space for qcbridge::gzip::inflate() in the tests below - static
// for the same reason g_responseBuffer is: too big for a test function's
// own stack. Sized against the largest asset's *raw* (uncompressed) size -
// the vendored Lit bundle is ~18 KB minified - not its gzipped size (the
// whole UI gzips to under 20 KB total, see web/ui/README.md); matches
// QcGzip.hpp's own kMaxInflatedSize headroom.
uint8_t g_inflateBuffer[32768];

// Issue #75: a minimal RFC 6455 client->server frame encoder - just enough
// to send the one message this firmware recognizes (resync). Client
// frames must be masked (section 5.3); the mask key can be anything, the
// server is required to undo it the same way regardless of value.
void sendWebSocketTextFrame(int sock, std::string_view payload) {
    zassert_true(payload.size() < 126, "test payload too long for this helper's framing");
    uint8_t frame[2 + 4 + 125];
    size_t pos = 0;
    frame[pos++] = 0x81;  // FIN=1, opcode=0x1 (text)
    frame[pos++] = 0x80 | static_cast<uint8_t>(payload.size());  // MASK=1, payload length
    const uint8_t maskKey[4] = {0x12, 0x34, 0x56, 0x78};
    for (const uint8_t byte : maskKey) {
        frame[pos++] = byte;
    }
    for (size_t i = 0; i < payload.size(); ++i) {
        frame[pos++] = static_cast<uint8_t>(payload[i]) ^ maskKey[i % 4];
    }
    const ssize_t sent = zsock_send(sock, frame, pos, 0);
    zassert_equal(sent, static_cast<ssize_t>(pos), "send() of websocket frame failed (%d)", errno);
}

void* setup(void) {
    g_qcWebPort = kServerPort;
    transport().startServer();
    // Let the HTTP server thread reach its accept loop before the first
    // connect(), and the worker thread drain its first wake.
    k_msleep(200);
    return nullptr;
}

}  // namespace

ZTEST_SUITE(wifi_web_http, NULL, setup, NULL, NULL, NULL);

// The page itself: a browser's navigation request has to come back as a
// complete HTTP/1.1 response with a Content-Length, or the browser never
// considers the document finished and never runs its script at all.
//
// Issue #88 / ADR 0011: "/" is now one of the embed step's generated
// static resources (firmware/app/tools/embed_assets.py over web/ui/),
// served pre-gzipped - the body below is a gzip stream, not plaintext, so
// this inflates it (qcbridge::gzip::inflate(), the same decoder the
// firmware uses for compressed device messages) to confirm the round
// trip actually produced the real page, not just well-formed headers.
ZTEST(wifi_web_http, test_index_page_is_served_gzipped_with_content_length) {
    publishSmokeTestCatalog();
    const int sock = connectToServer();
    sendRequest(sock, kNavigationRequest);
    const std::string_view response = receiveAll(sock);

    zassert_true(contains(response, "HTTP/1.1 200 OK"), "no 200 status line in: %.64s",
                 response.data());
    zassert_true(contains(response, "Content-Type: text/html"), "wrong content type");
    zassert_true(contains(response, "Content-Length: "), "no Content-Length");
    zassert_true(contains(response, "Content-Encoding: gzip"), "page isn't served pre-gzipped");

    const std::string_view gzipped = bodyOf(response);
    zassert_true(qcbridge::gzip::hasMagic(
                     std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(gzipped.data()),
                                              gzipped.size())),
                 "body doesn't start with the gzip magic");

    const auto inflated = qcbridge::gzip::inflate(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(gzipped.data()), gzipped.size()),
        std::span<uint8_t>(g_inflateBuffer));
    zassert_true(inflated.has_value(), "inflate failed: %s",
                 inflated.has_value() ? "" : qcbridge::gzip::toString(inflated.error()));
    const std::string_view page(reinterpret_cast<const char*>(inflated->data()), inflated->size());
    zassert_true(contains(page, "QC Remote"), "page title missing from inflated body");
    zassert_true(contains(page, "src/main.js"), "page isn't loading the UI's module entry");
    (void)zsock_close(sock);
}

// Issue #88 / ADR 0011: one of the UI's own served files, not the shell
// page - confirms the embed step's generated resources cover more than
// just "/", and that the vendored Lit bundle (web/ui/vendor/lit.js) is
// reachable at the path the page's own ES module imports use.
ZTEST(wifi_web_http, test_vendor_lit_js_is_served_gzipped) {
    const int sock = connectToServer();
    sendRequest(sock, kVendorLitRequest);
    const std::string_view response = receiveAll(sock);

    zassert_true(contains(response, "HTTP/1.1 200 OK"), "no 200 status line in: %.64s",
                 response.data());
    zassert_true(contains(response, "Content-Type: text/javascript"), "wrong content type");
    zassert_true(contains(response, "Content-Encoding: gzip"), "lit.js isn't served pre-gzipped");

    const std::string_view gzipped = bodyOf(response);
    const auto inflated = qcbridge::gzip::inflate(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(gzipped.data()), gzipped.size()),
        std::span<uint8_t>(g_inflateBuffer));
    zassert_true(inflated.has_value(), "inflate failed");
    const std::string_view bundle(reinterpret_cast<const char*>(inflated->data()), inflated->size());
    zassert_true(contains(bundle, "lit.dev"), "doesn't look like the vendored Lit bundle");
    (void)zsock_close(sock);
}

// Issue #88's own "done when": a build check that the generated header is
// present and its sizes are sane, independent of the HTTP tests above -
// this would fail to compile at all if firmware/app/tools/embed_assets.py
// hadn't run, and fail this assertion if it emitted something malformed.
ZTEST(wifi_web_http, test_generated_web_asset_table_is_sane) {
    size_t foundIndex = 0;
    size_t foundVendorLit = 0;
    for (const auto& asset : qcbridge::webassets::kAssets) {
        zassert_true(asset.gzippedLength > 0, "%s has a zero-length payload", asset.path);
        zassert_true(asset.gzippedLength >= 3, "%s too short to hold a gzip header", asset.path);
        const auto bytes = std::span<const uint8_t>(asset.gzipped, asset.gzippedLength);
        zassert_true(qcbridge::gzip::hasMagic(bytes), "%s isn't a gzip stream", asset.path);
        if (std::string_view(asset.path) == "/") {
            ++foundIndex;
            zassert_equal(std::string_view(asset.contentType), "text/html");
        }
        if (std::string_view(asset.path) == "/vendor/lit.js") {
            ++foundVendorLit;
        }
    }
    zassert_equal(foundIndex, 1u, "no (or more than one) \"/\" entry in the generated asset table");
    zassert_equal(foundVendorLit, 1u, "no vendored Lit bundle in the generated asset table");
}

// The regression the 5068403 smoke test hit: with the page open, the
// browser's own fetch('/api/presets.json') is usually reused on the same
// keep-alive connection the page came over.
ZTEST(wifi_web_http, test_presets_json_on_same_keep_alive_connection) {
    publishSmokeTestCatalog();
    const int sock = connectToServer();

    sendRequest(sock, kNavigationRequest);
    const std::string_view page = receiveAll(sock);
    zassert_true(contains(page, "HTTP/1.1 200 OK"), "page request failed");

    sendRequest(sock, kFetchRequest);
    const std::string_view json = receiveAll(sock);
    // A dynamic resource's status line is built from
    // RESPONSE_TEMPLATE_DYNAMIC_PART1, not the static resource's hardcoded
    // one, and it comes out as a bare "HTTP/1.1 200" - no space, no reason
    // phrase - unless CONFIG_HTTP_SERVER_COMPLETE_STATUS_PHRASES is on.
    // RFC 9112 section 4 requires the space, so this asserts the whole
    // well-formed line rather than just the status code.
    zassert_true(contains(json, "HTTP/1.1 200 OK\r\n"),
                 "malformed or missing status line for the JSON fetch");
    zassert_true(contains(json, "Transfer-Encoding: chunked"), "no transfer encoding");
    zassert_true(contains(json, "Content-Type: application/json"), "wrong content type");
    zassert_true(contains(json, "\"presetCount\":49"), "catalog not in the response");
    zassert_true(contains(json, "My Presets"), "first folder missing");
    zassert_true(contains(json, "Practice"), "second folder missing");
    zassert_true(contains(json, "Downloads"), "third folder missing");
    zassert_true(contains(json, "\"incomplete\":false"), "listing reported as incomplete");
    (void)zsock_close(sock);
}

// The other shape a browser uses: a separate connection for the fetch.
ZTEST(wifi_web_http, test_presets_json_on_a_fresh_connection) {
    publishSmokeTestCatalog();
    const int pageSock = connectToServer();
    sendRequest(pageSock, kNavigationRequest);
    (void)receiveAll(pageSock);

    const int fetchSock = connectToServer();
    sendRequest(fetchSock, kFetchRequest);
    const std::string_view json = receiveAll(fetchSock);
    zassert_true(contains(json, "HTTP/1.1 200 OK\r\n"),
                 "malformed or missing status line for the JSON fetch");
    zassert_true(contains(json, "\"presetCount\":49"), "catalog not in the response");
    (void)zsock_close(fetchSock);
    (void)zsock_close(pageSock);
}

// Before this round, catalogJson_ was re-encoded only by the worker
// thread's broadcast(), so a GET that beat that thread served the startup
// placeholder ({"presetCount":0,...,"incomplete":true}) instead of the
// listing that had already been published. Publishing and fetching back to
// back is the case that used to be racy.
ZTEST(wifi_web_http, test_presets_json_reflects_the_latest_publish_immediately) {
    publishSmokeTestCatalog();
    const int sock = connectToServer();
    sendRequest(sock, kFetchRequest);
    const std::string_view json = receiveAll(sock);
    zassert_true(contains(json, "HTTP/1.1 200 OK\r\n"),
                 "malformed or missing status line for the JSON fetch");
    zassert_true(contains(json, "\"presetCount\":49"), "GET served a stale catalog");
    (void)zsock_close(sock);
}

// A WebSocket upgrade has to be accepted, and - the other half of the
// smoke test's "nothing arrives over the WebSocket" - the client has to
// receive the catalog the firmware already holds without waiting for the
// next publish.
ZTEST(wifi_web_http, test_websocket_upgrade_pushes_the_current_catalog) {
    publishSmokeTestCatalog();
    // Drain the publish's own broadcast so what this test reads can only
    // be the push triggered by the upgrade itself.
    k_msleep(200);

    const int sock = connectToServer();
    sendRequest(sock, kWebsocketRequest);
    const std::string_view response = receiveAll(sock);

    zassert_true(contains(response, "HTTP/1.1 101 Switching Protocols"),
                 "websocket upgrade refused: %s",
                 response.empty() ? "<nothing received>" : response.data());
    // RFC 6455's own example key hashes to this accept value.
    zassert_true(contains(response, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="),
                 "wrong Sec-WebSocket-Accept");
    zassert_true(contains(response, "\"presetCount\":49"),
                 "no catalog pushed to the new websocket client");
    // Issue #75 / ADR 0011: the push is wrapped in the versioned envelope,
    // as a "snapshot" (never a "patch" - a newly-connected client has no
    // prior state to patch), with its own seq starting fresh at 0.
    zassert_true(contains(response, R"("v":1,"t":"snapshot","seq":0,"catalog":)"),
                 "catalog push isn't wrapped in a v1 snapshot envelope: %s", response.data());
    (void)zsock_close(sock);
    // Issue #75: CONFIG_WEBSOCKET_MAX_CONTEXTS=1 means the next test's own
    // connect only succeeds once the worker thread's own poll cycle
    // (kPollIntervalMs, 1000ms) has noticed this socket closed and freed
    // the one slot - give it that cycle before moving on.
    k_msleep(1100);
}

// Issue #75: a preset-change event with no catalog change must reach the
// client as a lean "patch" (just the envelope plus lastChange), not a
// second full catalog resend - and its seq must continue from the
// snapshot's.
ZTEST(wifi_web_http, test_preset_change_is_pushed_as_a_patch_not_a_full_catalog) {
    publishSmokeTestCatalog();
    k_msleep(200);

    const int sock = connectToServer();
    sendRequest(sock, kWebsocketRequest);
    const std::string_view snapshot = receiveAll(sock);
    zassert_true(contains(snapshot, R"("t":"snapshot","seq":0,)"), "no initial snapshot: %s",
                 snapshot.data());

    transport().publishPresetChange(QcPresetChangeEvent{.command = 15, .count = 3});
    k_msleep(200);
    const std::string_view patch = receiveAll(sock);

    zassert_true(contains(patch, R"({"v":1,"t":"patch","seq":1,"lastChange":{"command":15,)"
                                 R"("count":3}})"),
                 "preset change wasn't pushed as the expected patch envelope: %s", patch.data());
    zassert_false(contains(patch, "folders"), "a patch must not resend the full catalog: %s",
                  patch.data());
    (void)zsock_close(sock);
    k_msleep(1100);  // See test_websocket_upgrade_pushes_the_current_catalog's own comment.
}

// Issue #75 / ADR 0011: without a tick, an idle rig and a half-open socket
// look the same. CONFIG_QC_WIFI_WEB_TICK_INTERVAL_MS is overridden (this
// file's own #define above) so the test doesn't have to wait out the real
// (5s) production cadence - see that #define's own comment for why it's
// still kept above kRecvTimeoutSeconds rather than as short as possible.
ZTEST(wifi_web_http, test_tick_is_sent_when_nothing_else_is) {
    publishSmokeTestCatalog();
    k_msleep(200);

    const int sock = connectToServer();
    sendRequest(sock, kWebsocketRequest);
    const std::string_view snapshot = receiveAll(sock);
    zassert_true(contains(snapshot, R"("t":"snapshot","seq":0,)"), "no initial snapshot: %s",
                 snapshot.data());

    // Comfortably past kTickIntervalMs (2500ms, this file's own #define
    // above) plus the worker's own poll granularity (kPollIntervalMs,
    // 1000ms) - sleeping *before* calling receiveAll() rather than
    // relying on it to wait the tick out, since a tick this slow would
    // otherwise exceed receiveAll()'s own per-call quiet timeout
    // (kRecvTimeoutSeconds, 2s) before ever arriving.
    k_msleep(3800);
    const std::string_view tick = receiveAll(sock);
    zassert_true(contains(tick, R"({"v":1,"t":"tick","seq":1})"), "no tick heartbeat arrived: %s",
                 tick.data());
    (void)zsock_close(sock);
    k_msleep(1100);  // See test_websocket_upgrade_pushes_the_current_catalog's own comment.
}

// Issue #75: the client's own resync request (sent after it notices a seq
// gap) must get a fresh "snapshot" back, not be silently ignored.
ZTEST(wifi_web_http, test_resync_request_triggers_a_fresh_snapshot) {
    publishSmokeTestCatalog();
    k_msleep(200);

    const int sock = connectToServer();
    sendRequest(sock, kWebsocketRequest);
    const std::string_view snapshot = receiveAll(sock);
    zassert_true(contains(snapshot, R"("t":"snapshot","seq":0,)"), "no initial snapshot: %s",
                 snapshot.data());

    sendWebSocketTextFrame(sock, R"({"v":1,"t":"resync"})");
    // Resync detection happens on the worker's own poll cadence
    // (kPollIntervalMs, 1000ms) - give it comfortable margin.
    k_msleep(1500);
    const std::string_view resynced = receiveAll(sock);
    zassert_true(contains(resynced, R"("t":"snapshot")"), "resync didn't produce a fresh snapshot: %s",
                 resynced.data());
    zassert_true(contains(resynced, "\"presetCount\":49"), "fresh snapshot missing the catalog: %s",
                 resynced.data());
    (void)zsock_close(sock);
    k_msleep(1100);  // See test_websocket_upgrade_pushes_the_current_catalog's own comment.
}
