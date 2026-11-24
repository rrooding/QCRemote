// Issue #71: drives QcWifiWebTransport.hpp's real HTTP/WebSocket resources
// over a real TCP socket on Zephyr's loopback interface, doing what a
// browser actually does: GET / and the page's own assets (on one keep-alive
// connection, and over several in parallel), plus a WebSocket upgrade.
// Issue #88 / ADR 0011 added coverage for the embed step's generated
// resources (the real web UI, pre-gzipped) alongside these.
//
// Issue #99 made this suite thin on purpose: what goes over the WebSocket
// is decided by QcWebApiSession.hpp, tested on its own (tests/
// web_api_session). This suite checks only what that can't - the HTTP side
// (no more /api/presets.json, Cache-Control: no-cache on every asset, the
// build id stamped into the page), and the transport's own handling of the
// socket: frames reach the page, frames from the page reach the session,
// and a second page replaces the first with close code 4001.
//
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
// the two above do. Kept *above* receiveAll()'s own kRecvTimeoutSeconds
// (2000ms): every WebSocket test reads with receiveAll(), which decides a
// response is complete once the socket goes quiet for kRecvTimeoutSeconds -
// and once a page is connected, the Board never stays quiet for longer than
// the tick interval (that's the tick's whole point).
#define CONFIG_QC_WIFI_WEB_TICK_INTERVAL_MS 2500

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// The header set Safari sends for a fetch from the page's own script -
// deliberately not a minimal hand-written request, since the point is to
// exercise the server with what a browser actually sends (header count and
// sizes included: CONFIG_HTTP_SERVER_CLIENT_BUFFER_SIZE is 256, so these
// span several reads).
#define QC_BROWSER_HEADERS                                                                         \
    "Host: 192.168.1.233\r\n"                                                                      \
    "Accept: */*\r\n"                                                                              \
    "Referer: http://192.168.1.233/\r\n"                                                           \
    "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "            \
    "(KHTML, like Gecko) Version/18.0 Safari/605.1.15\r\n"                                         \
    "Accept-Language: en-GB,en;q=0.9\r\n"                                                          \
    "Accept-Encoding: gzip, deflate\r\n"                                                           \
    "Connection: keep-alive\r\n"                                                                   \
    "\r\n"

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

constexpr const char kVendorLitRequest[] = "GET /vendor/lit.js HTTP/1.1\r\n" QC_BROWSER_HEADERS;
constexpr const char kBuildJsRequest[] = "GET /src/build.js HTTP/1.1\r\n" QC_BROWSER_HEADERS;
// Issue #99 / ADR 0012 decision 8: the WebSocket is the only API.
constexpr const char kOldCatalogRequest[] = "GET /api/presets.json HTTP/1.1\r\n" QC_BROWSER_HEADERS;

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
// full, and returns what arrived as a NUL-terminated string. Reading to
// quiet rather than parsing lengths is what lets a *malformed* response be
// asserted against too - a response that never arrives, or arrives
// without a status line, is exactly the failure being chased.
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

// Issue #99: assets are dynamic resources now (so they can carry
// Cache-Control), which Zephyr always sends with Transfer-Encoding:
// chunked, as one chunk: "<hex length>\r\n<body>\r\n0\r\n\r\n" after the
// headers (RFC 9112 section 7.1). This slices that one chunk's body out.
constexpr std::string_view kLastChunk = "\r\n0\r\n\r\n";

std::string_view bodyOf(std::string_view httpResponse) {
    constexpr std::string_view kHeaderEnd = "\r\n\r\n";
    const size_t headerEnd = httpResponse.find(kHeaderEnd);
    zassert_true(headerEnd != std::string_view::npos, "no end-of-headers marker in response");
    const std::string_view chunked = httpResponse.substr(headerEnd + kHeaderEnd.size());
    const size_t sizeLineEnd = chunked.find("\r\n");
    zassert_true(sizeLineEnd != std::string_view::npos, "no chunk size line");
    const size_t length = strtoul(chunked.data(), nullptr, 16);
    const std::string_view rest = chunked.substr(sizeLineEnd + 2);
    zassert_true(rest.size() == length + kLastChunk.size() && rest.ends_with(kLastChunk),
                 "expected one %zu-byte chunk and the last chunk, got %zu bytes", length, rest.size());
    return rest.substr(0, length);
}

// Scratch space for qcbridge::gzip::inflate() in the tests below - static
// for the same reason g_responseBuffer is: too big for a test function's
// own stack. Sized against the largest asset's *raw* (uncompressed) size -
// the vendored Lit bundle is ~18 KB minified.
uint8_t g_inflateBuffer[32768];

std::string_view inflatedBodyOf(std::string_view httpResponse) {
    const std::string_view gzipped = bodyOf(httpResponse);
    const auto bytes =
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(gzipped.data()), gzipped.size());
    zassert_true(qcbridge::gzip::hasMagic(bytes), "body doesn't start with the gzip magic");
    const auto inflated = qcbridge::gzip::inflate(bytes, std::span<uint8_t>(g_inflateBuffer));
    zassert_true(inflated.has_value(), "inflate failed: %s",
                 inflated.has_value() ? "" : qcbridge::gzip::toString(inflated.error()));
    return std::string_view(reinterpret_cast<const char*>(inflated->data()), inflated->size());
}

// A minimal RFC 6455 client->server text frame. Client frames must be
// masked (section 5.3); the mask key can be anything.
uint8_t g_frameBuffer[2 + 2 + 4 + 512];

void sendWebSocketTextFrame(int sock, std::string_view payload) {
    zassert_true(payload.size() <= 512, "test payload too long for this helper");
    size_t pos = 0;
    g_frameBuffer[pos++] = 0x81;  // FIN=1, opcode=0x1 (text)
    if (payload.size() < 126) {
        g_frameBuffer[pos++] = 0x80 | static_cast<uint8_t>(payload.size());
    } else {
        g_frameBuffer[pos++] = 0x80 | 126;  // MASK=1, 16-bit length follows
        g_frameBuffer[pos++] = static_cast<uint8_t>(payload.size() >> 8);
        g_frameBuffer[pos++] = static_cast<uint8_t>(payload.size() & 0xff);
    }
    const uint8_t maskKey[4] = {0x12, 0x34, 0x56, 0x78};
    for (const uint8_t byte : maskKey) {
        g_frameBuffer[pos++] = byte;
    }
    for (size_t i = 0; i < payload.size(); ++i) {
        g_frameBuffer[pos++] = static_cast<uint8_t>(payload[i]) ^ maskKey[i % 4];
    }
    const ssize_t sent = zsock_send(sock, g_frameBuffer, pos, 0);
    zassert_equal(sent, static_cast<ssize_t>(pos), "send() of websocket frame failed (%d)", errno);
}

// Opens /ws and returns the socket, having read the upgrade response and
// the snapshot that follows it (left in g_responseBuffer).
int openPage(std::string_view& received) {
    const int sock = connectToServer();
    sendRequest(sock, kWebsocketRequest);
    received = receiveAll(sock);
    zassert_true(contains(received, "HTTP/1.1 101 Switching Protocols"), "websocket upgrade refused: %s",
                 received.empty() ? "<nothing received>" : received.data());
    return sock;
}

// Issue #88 (PR #92's hardware blocker): a browser does not fetch the
// page's files one after another - Safari and Chrome both open up to six
// TCP connections per host and fetch over all of them at once. The board
// served every asset correctly when curl asked for them sequentially, and
// reset/refused almost all of them as soon as two or more connections were
// open at the same time, so the page never got its module graph. The
// tests at the end of this file are that request pattern.
//
// Six, because that is the real per-host cap both browsers use (and the
// number the owner's curl check reproduces the failure with); the UI ships
// more assets than that, so a browser fetches them in two waves over these
// six connections, which is what test_every_page_asset_loads_over_parallel_
// connections below does.
constexpr size_t kBrowserConnections = 6;

// Like connectToServer()/sendRequest() above but without the zassert: the
// failure being tested is one where connect() and send() themselves fail
// (the server had closed its listening socket), and a zassert there would
// abort the test before it could say how many of the connections got
// through, and before it could close the ones that did.
int tryConnectToServer(int recvTimeoutSeconds = kRecvTimeoutSeconds) {
    const int sock = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);
    if (sock < 0) {
        return -1;
    }

    const timeval timeout{.tv_sec = recvTimeoutSeconds, .tv_usec = 0};
    if (zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &timeout, sizeof(timeout)) <
        0) {
        (void)zsock_close(sock);
        return -1;
    }

    net_sockaddr_in address{};
    address.sin_family = NET_AF_INET;
    address.sin_port = net_htons(kServerPort);
    if (zsock_inet_pton(NET_AF_INET, kServerAddress, &address.sin_addr.s_addr) != 1) {
        (void)zsock_close(sock);
        return -1;
    }
    if (zsock_connect(sock, reinterpret_cast<net_sockaddr*>(&address), sizeof(address)) < 0) {
        (void)zsock_close(sock);
        return -1;
    }
    return sock;
}

// One asset request, with the same header set kVendorLitRequest uses, for
// an arbitrary path from the generated asset table. Reused for every
// request below, which is safe because send() has copied it by the time
// this returns.
char g_requestBuffer[512];

// Sends a literal, already-formed request (the WebSocket upgrade), without
// sendRequest()'s zassert - same reason tryConnectToServer() exists.
bool sendAssetRequestRaw(int sock, const char* request) {
    const size_t length = strlen(request);
    return zsock_send(sock, request, length, 0) == static_cast<ssize_t>(length);
}

bool sendAssetRequest(int sock, const char* path) {
    const int length =
        snprintf(g_requestBuffer, sizeof(g_requestBuffer), "GET %s HTTP/1.1\r\n" QC_BROWSER_HEADERS, path);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(g_requestBuffer)) {
        return false;
    }
    return zsock_send(sock, g_requestBuffer, static_cast<size_t>(length), 0) ==
           static_cast<ssize_t>(length);
}

// What one parallel asset fetch came back as. Only the start of the
// response is kept (enough for the status line) plus the body's byte
// count - with six connections in flight, keeping every body would cost
// more static RAM than this suite has any reason to, and the assertions
// below are about *whether* each asset arrived complete, not about its
// bytes (which the single-connection tests above already inflate and
// check).
struct AssetFetch {
    const char* path;
    bool complete;
    int error;  // errno from the failing recv/send, or 0
    char head[80];
    size_t expectedBodyLength;
    size_t bodyLength;
};

// Drain space for the bodies read by readAssetResponse() - one shared
// buffer, since responses are read one socket at a time.
char g_drainBuffer[512];

// Reads one complete asset response: the headers, the chunk size line, then
// exactly that many body bytes plus the last chunk.
//
// Deliberately not receiveAll(): that one reads until the socket goes
// quiet, which on these keep-alive connections (the server never closes
// them on its own - a browser's connections stay open) costs
// kRecvTimeoutSeconds per connection and, worse, cannot tell a response
// that arrived in full from one that was cut short. Reading to the
// declared length makes "this connection's asset never finished" an
// immediate, attributable result.
bool readAssetResponse(int sock, AssetFetch& out) {
    char header[384];
    size_t headerLength = 0;
    size_t headerEnd = std::string_view::npos;
    size_t sizeLineEnd = std::string_view::npos;

    while (sizeLineEnd == std::string_view::npos && headerLength < sizeof(header)) {
        const ssize_t received =
            zsock_recv(sock, header + headerLength, sizeof(header) - headerLength, 0);
        if (received <= 0) {
            out.error = (received < 0) ? errno : 0;
            break;
        }
        headerLength += static_cast<size_t>(received);
        const std::string_view view(header, headerLength);
        if (headerEnd == std::string_view::npos) {
            const size_t pos = view.find("\r\n\r\n");
            if (pos != std::string_view::npos) {
                headerEnd = pos + 4;
            }
        }
        if (headerEnd != std::string_view::npos) {
            const size_t pos = view.find("\r\n", headerEnd);
            if (pos != std::string_view::npos) {
                sizeLineEnd = pos + 2;
            }
        }
    }

    const size_t kept = headerLength < sizeof(out.head) - 1 ? headerLength : sizeof(out.head) - 1;
    memcpy(out.head, header, kept);
    out.head[kept] = '\0';
    // So a status line in the message reads as one line even when the
    // headers that follow it got cut off mid-way.
    for (size_t i = 0; i < kept; ++i) {
        if (out.head[i] == '\r' || out.head[i] == '\n') {
            out.head[i] = ' ';
        }
    }

    if (sizeLineEnd == std::string_view::npos) {
        return false;
    }

    out.expectedBodyLength = strtoul(header + headerEnd, nullptr, 16);
    const size_t total = sizeLineEnd + out.expectedBodyLength + kLastChunk.size();
    size_t received = headerLength;
    while (received < total) {
        size_t want = total - received;
        if (want > sizeof(g_drainBuffer)) {
            want = sizeof(g_drainBuffer);
        }
        const ssize_t got = zsock_recv(sock, g_drainBuffer, want, 0);
        if (got <= 0) {
            out.error = (got < 0) ? errno : 0;
            out.bodyLength = received - sizeLineEnd;
            return false;
        }
        received += static_cast<size_t>(got);
    }
    out.bodyLength = out.expectedBodyLength;
    out.complete = received == total;
    return out.complete;
}

// Sends one asset request per already-open connection, then reads them
// all back. Requests go out before any response is read, so all of them
// are genuinely in flight at once - a send/read pair per connection would
// serialise the thing under test.
void fetchAssetsInParallel(const int* sockets, AssetFetch* fetches, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (sockets[i] < 0) {
            fetches[i].error = ENOTCONN;
            continue;
        }
        if (!sendAssetRequest(sockets[i], fetches[i].path)) {
            fetches[i].error = errno;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (sockets[i] < 0 || fetches[i].error != 0) {
            continue;
        }
        (void)readAssetResponse(sockets[i], fetches[i]);
    }
}

// Turns the per-asset outcomes into one assertion message that names
// every asset that didn't come back, rather than failing on whichever
// one happened to be checked first.
void assertAllFetchesSucceeded(const AssetFetch* fetches, size_t count, const char* what) {
    size_t failed = 0;
    for (size_t i = 0; i < count; ++i) {
        const AssetFetch& fetch = fetches[i];
        const bool ok = fetch.complete &&
                        std::string_view(fetch.head).find("HTTP/1.1 200 OK") == 0;
        if (ok) {
            continue;
        }
        ++failed;
        TC_PRINT("%s: %s FAILED - errno %d, %zu/%zu body bytes, response starts: \"%s\"\n", what,
                 fetch.path, fetch.error, fetch.bodyLength, fetch.expectedBodyLength,
                 fetch.head);
    }
    zassert_equal(failed, 0u, "%s: %zu of %zu assets failed over %zu parallel connections", what,
                  failed, count, kBrowserConnections);
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
// complete HTTP/1.1 response, or the browser never considers the document
// finished and never runs its script at all.
//
// Issue #88 / ADR 0011: "/" is one of the embed step's generated resources
// (firmware/app/tools/embed_assets.py over web/ui/), served pre-gzipped -
// the body is a gzip stream, so this inflates it (qcbridge::gzip::inflate(),
// the same decoder the firmware uses for compressed device messages) to
// confirm the round trip actually produced the real page.
//
// Issue #99 / docs/api: with Cache-Control: no-cache, so a reload after a
// firmware update revalidates instead of running cached old JavaScript.
ZTEST(wifi_web_http, test_index_page_is_served_gzipped_and_not_cached) {
    const int sock = connectToServer();
    sendRequest(sock, kNavigationRequest);
    const std::string_view response = receiveAll(sock);

    zassert_true(contains(response, "HTTP/1.1 200 OK\r\n"), "no 200 status line in: %.64s",
                 response.data());
    zassert_true(contains(response, "Content-Type: text/html\r\n"), "wrong content type");
    zassert_true(contains(response, "Content-Encoding: gzip\r\n"), "page isn't served pre-gzipped");
    zassert_true(contains(response, "Cache-Control: no-cache\r\n"), "page may be cached");

    const std::string_view page = inflatedBodyOf(response);
    zassert_true(contains(page, "QC Remote"), "page title missing from inflated body");
    zassert_true(contains(page, "src/main.js"), "page isn't loading the UI's module entry");
    (void)zsock_close(sock);
}

// Issue #88 / ADR 0011: one of the UI's own served files, not the shell
// page - and the biggest one, so the whole body arriving in one piece is
// worth checking.
ZTEST(wifi_web_http, test_vendor_lit_js_is_served_gzipped_and_not_cached) {
    const int sock = connectToServer();
    sendRequest(sock, kVendorLitRequest);
    const std::string_view response = receiveAll(sock);

    zassert_true(contains(response, "HTTP/1.1 200 OK\r\n"), "no 200 status line in: %.64s",
                 response.data());
    zassert_true(contains(response, "Content-Type: text/javascript\r\n"), "wrong content type");
    zassert_true(contains(response, "Content-Encoding: gzip\r\n"), "lit.js isn't served pre-gzipped");
    zassert_true(contains(response, "Cache-Control: no-cache\r\n"), "lit.js may be cached");
    zassert_true(contains(inflatedBodyOf(response), "lit.dev"), "doesn't look like the vendored Lit bundle");
    (void)zsock_close(sock);
}

// Issue #99 / ADR 0012: the page's copy of the build id is the firmware's,
// stamped into web/ui/src/build.js by the same embed step.
ZTEST(wifi_web_http, test_the_page_gets_the_firmwares_build_id) {
    const int sock = connectToServer();
    sendRequest(sock, kBuildJsRequest);
    const std::string_view buildJs = inflatedBodyOf(receiveAll(sock));

    static char expected[64];
    (void)snprintf(expected, sizeof(expected), "export const BUILD = '%.*s';",
                   static_cast<int>(kBuildId.size()), kBuildId.data());
    zassert_true(contains(buildJs, expected), "build.js doesn't carry %s: %.*s", expected,
                 static_cast<int>(buildJs.size()), buildJs.data());
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
        zassert_true(asset.gzipped.size() >= 3, "%s too short to hold a gzip header", asset.path);
        zassert_true(qcbridge::gzip::hasMagic(asset.gzipped), "%s isn't a gzip stream", asset.path);
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

// The browser reuses the connection the page came over for the page's own
// files (keep-alive); a server that drops it in between is the regression
// prj.conf's CONFIG_HTTP_SERVER_CLIENT_INACTIVITY_TIMEOUT comment is about.
ZTEST(wifi_web_http, test_two_assets_on_one_keep_alive_connection) {
    const int sock = connectToServer();

    sendRequest(sock, kNavigationRequest);
    zassert_true(contains(receiveAll(sock), "HTTP/1.1 200 OK\r\n"), "page request failed");

    sendRequest(sock, kVendorLitRequest);
    const std::string_view response = receiveAll(sock);
    zassert_true(contains(response, "HTTP/1.1 200 OK\r\n"), "second request on the same connection failed");
    zassert_true(contains(inflatedBodyOf(response), "lit.dev"), "second asset is wrong");
    (void)zsock_close(sock);
}

// Issue #99 / ADR 0012 decision 8: the WebSocket is the only API, and the
// catalog endpoint (and its 16 KB buffer) is gone.
ZTEST(wifi_web_http, test_the_old_catalog_endpoint_is_gone) {
    publishSmokeTestCatalog();
    const int sock = connectToServer();
    sendRequest(sock, kOldCatalogRequest);
    const std::string_view response = receiveAll(sock);

    zassert_true(contains(response, "HTTP/1.1 404"), "expected a 404, got: %.64s", response.data());
    zassert_false(contains(response, "presetCount"), "the catalog is still served over HTTP");
    (void)zsock_close(sock);
}

// The other half of the 5068403 smoke test's "nothing arrives over the
// WebSocket": a page that connects after the Catalog was published gets
// it right away, in a docs/api snapshot carrying this firmware's build id.
ZTEST(wifi_web_http, test_a_new_page_gets_a_snapshot_right_away) {
    publishSmokeTestCatalog();
    k_msleep(200);

    std::string_view received;
    const int sock = openPage(received);

    // RFC 6455's own example key hashes to this accept value.
    zassert_true(contains(received, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="),
                 "wrong Sec-WebSocket-Accept");
    static char expectedStart[96];
    (void)snprintf(expectedStart, sizeof(expectedStart), R"({"t":"snapshot","seq":0,"build":"%.*s","catalog":)",
                   static_cast<int>(kBuildId.size()), kBuildId.data());
    zassert_true(contains(received, expectedStart), "no snapshot: %s", received.data());
    zassert_true(contains(received, "\"presetCount\":49"), "snapshot without the published Catalog");
    (void)zsock_close(sock);
}

// What QcSession publishes reaches the open page.
ZTEST(wifi_web_http, test_a_published_current_preset_reaches_the_page_as_a_patch) {
    transport().publishCurrentPreset(std::nullopt);
    publishSmokeTestCatalog();
    k_msleep(200);

    std::string_view received;
    const int sock = openPage(received);
    zassert_true(contains(received, R"("current":{"index":null,"name":null,"scene":null})"),
                 "snapshot with a stale Current preset: %s", received.data());

    QcCatalogSnapshot::PresetEntry verse;
    verse.index = 3;
    verse.name = "Verse 1";
    transport().publishCurrentPreset(verse);
    received = receiveAll(sock);
    zassert_true(contains(received, R"({"t":"patch","seq":1,"current":{"index":3,"name":"Verse 1","scene":null}})"),
                 "no current patch: %s", received.data());
    (void)zsock_close(sock);
}

// Frames from the page reach the session: a resync is answered with a
// fresh snapshot.
ZTEST(wifi_web_http, test_a_resync_from_the_page_gets_a_fresh_snapshot) {
    publishSmokeTestCatalog();
    k_msleep(200);

    std::string_view received;
    const int sock = openPage(received);

    sendWebSocketTextFrame(sock, R"({"t":"resync"})");
    received = receiveAll(sock);
    zassert_true(contains(received, R"({"t":"snapshot","seq":0,)"), "resync didn't produce a snapshot: %s",
                 received.data());
    (void)zsock_close(sock);
}

// docs/api: a page -> Board frame is at most 256 bytes. The transport reads
// a longer one to its end (so the next frame still parses) and the session
// refuses it.
ZTEST(wifi_web_http, test_a_frame_over_256_bytes_is_refused_and_the_next_one_still_works) {
    publishSmokeTestCatalog();
    k_msleep(200);

    std::string_view received;
    const int sock = openPage(received);

    static char oversized[300];
    memset(oversized, ' ', sizeof(oversized));
    const std::string_view resync = R"({"t":"resync"})";
    memcpy(oversized, resync.data(), resync.size());
    sendWebSocketTextFrame(sock, std::string_view(oversized, sizeof(oversized)));
    received = receiveAll(sock);
    zassert_true(contains(received, R"({"t":"error","seq":1,"code":"badRequest","message":"frame too large"})"),
                 "oversized frame not refused: %s", received.data());

    sendWebSocketTextFrame(sock, resync);
    received = receiveAll(sock);
    zassert_true(contains(received, R"({"t":"snapshot","seq":0,)"), "the frame after it was lost: %s",
                 received.data());
    (void)zsock_close(sock);
}

// Issue #99 / docs/api's "One page at a time": the newest page wins, and
// the one it replaced is closed with 4001 so it stops reconnecting.
ZTEST(wifi_web_http, test_a_second_page_replaces_the_first_with_close_code_4001) {
    publishSmokeTestCatalog();
    k_msleep(200);

    std::string_view received;
    const int first = openPage(received);
    zassert_true(contains(received, R"({"t":"snapshot","seq":0,)"), "first page got no snapshot");

    const int second = openPage(received);
    zassert_true(contains(received, R"({"t":"snapshot","seq":0,)"), "second page got no snapshot: %s",
                 received.data());

    // An unmasked close frame (FIN + opcode 8), a 2-byte payload, 4001.
    constexpr char kClose4001[] = {static_cast<char>(0x88), 0x02, 0x0f, static_cast<char>(0xa1)};
    received = receiveAll(first);
    zassert_true(contains(received, std::string_view(kClose4001, sizeof(kClose4001))),
                 "first page wasn't closed with 4001 (%zu bytes received)", received.size());

    // The second page is the live one now.
    sendWebSocketTextFrame(second, R"({"t":"resync"})");
    received = receiveAll(second);
    zassert_true(contains(received, R"({"t":"snapshot","seq":0,)"), "second page isn't live: %s",
                 received.data());

    (void)zsock_close(first);
    (void)zsock_close(second);
}

// PR #92's hardware blocker, reproduced off-board: six connections open at
// the same time, one asset requested on each. Before the fix this failed
// with the exact shapes the owner's curl check saw (connection reset,
// empty reply, connection refused) - Zephyr's HTTP server polls
// 1 + listen + CONFIG_HTTP_SERVER_MAX_CLIENTS descriptors in one
// zsock_poll() call, and zvfs_poll_internal() can only build
// CONFIG_ZVFS_POLL_MAX k_poll_events for them. Once more descriptors were
// active than that, poll() failed with ENOMEM, and http_server_run()
// treats a poll() failure as fatal: close_all_sockets() drops every live
// connection *and* the listening socket, then the server restarts
// CONFIG_HTTP_SERVER_RESTART_DELAY later. One connection fits in the
// stock three poll slots (eventfd + listen + one client), which is why
// sequential requests always worked and the old inline page never showed
// this. See firmware/app/prj.conf's own ZVFS_POLL_MAX comment.
ZTEST(wifi_web_http, test_parallel_connections_each_get_their_asset) {
    int sockets[kBrowserConnections];
    AssetFetch fetches[kBrowserConnections]{};
    size_t connected = 0;

    // Connect every socket before sending anything: these have to be open
    // simultaneously for this to be the test it claims to be.
    for (size_t i = 0; i < kBrowserConnections; ++i) {
        sockets[i] = tryConnectToServer();
        if (sockets[i] >= 0) {
            ++connected;
        }
        fetches[i].path = qcbridge::webassets::kAssets[i].path;
    }

    fetchAssetsInParallel(sockets, fetches, kBrowserConnections);

    for (size_t i = 0; i < kBrowserConnections; ++i) {
        if (sockets[i] >= 0) {
            (void)zsock_close(sockets[i]);
        }
    }

    zassert_equal(connected, kBrowserConnections, "only %zu of %zu connections were accepted",
                  connected, kBrowserConnections);
    assertAllFetchesSucceeded(fetches, kBrowserConnections, "parallel asset fetch");
}

// The whole page load, the way the browser actually does it: all of the
// UI's assets over six connections that stay open across both waves
// (keep-alive, exactly as the request headers above ask for). This is the
// case the owner's Safari test failed on - "/" came back but
// /src/main.js and its siblings didn't, so the module graph never
// resolved and the UI never started.
ZTEST(wifi_web_http, test_every_page_asset_loads_over_parallel_connections) {
    constexpr size_t kAssetCount = ARRAY_SIZE(qcbridge::webassets::kAssets);
    BUILD_ASSERT(kAssetCount > kBrowserConnections,
                 "this test is about a page needing more than one wave of fetches; if the UI "
                 "ever ships fewer assets than a browser opens connections, drop it");

    int sockets[kBrowserConnections];
    size_t connected = 0;
    for (size_t i = 0; i < kBrowserConnections; ++i) {
        sockets[i] = tryConnectToServer();
        if (sockets[i] >= 0) {
            ++connected;
        }
    }

    AssetFetch fetches[kAssetCount]{};
    for (size_t i = 0; i < kAssetCount; ++i) {
        fetches[i].path = qcbridge::webassets::kAssets[i].path;
    }

    for (size_t done = 0; done < kAssetCount; done += kBrowserConnections) {
        const size_t wave =
            (kAssetCount - done) < kBrowserConnections ? kAssetCount - done : kBrowserConnections;
        fetchAssetsInParallel(sockets, fetches + done, wave);
    }

    for (size_t i = 0; i < kBrowserConnections; ++i) {
        if (sockets[i] >= 0) {
            (void)zsock_close(sockets[i]);
        }
    }

    zassert_equal(connected, kBrowserConnections, "only %zu of %zu connections were accepted",
                  connected, kBrowserConnections);
    assertAllFetchesSucceeded(fetches, kAssetCount, "full page load");
}

// PR #92 follow-up, the owner's second hardware round: with the poll fix in
// place Chrome loaded the page fine, but Safari was very slow and some
// modules still never arrived - and the coordinator's parallel curl check
// came back *serialised in ~1s steps* (0.115, 0.115, 1.12, 1.16, 2.04,
// 2.05, 3.07, 3.09, 4.09s) with browser tabs still open. That ladder is
// client-side SYN retransmission, not a timer on the board: once the
// server has no free client slot it stops accepting (handle_listen_pollin()
// zeroes the listening socket's poll events at num_clients >= concurrent),
// and once the listening socket's own TCP backlog is also full Zephyr drops
// further SYNs outright (tcp.c's tcp_backlog_is_full() - "same behavior as
// on Linux", retransmitted SYNs may succeed later). The client then retries
// about once a second, and gets in only as fast as slots free up.
//
// What fills those slots is the thing this test is about: a browser that
// has *finished* loading the page does not close its connections. Zephyr's
// server never sends Connection: close, so each one holds a client slot
// until CONFIG_HTTP_SERVER_CLIENT_INACTIVITY_TIMEOUT expires. So the
// question was never "how many connections does one page load need" - it is
// "how many are still parked when the next page load starts". Two browsers
// open at once (the owner had Chrome and Safari), or one browser reloading,
// needs twice the per-browser cap.
//
// Also asserted here, because ADR 0011 claimed it and the fix depends on
// it: a WebSocket does *not* hold an HTTP client slot.
// handle_http1_to_websocket_upgrade() calls websocket_register() and then
// http_server_release_client() (subsys/net/lib/http/http_server_ws.c), so
// the live-update socket is handed to the application and the slot goes
// back. Chrome's open WebSocket is therefore not part of the pressure; its
// six idle keep-alive connections are.
ZTEST(wifi_web_http, test_a_second_browsers_page_load_is_not_locked_out) {
    publishSmokeTestCatalog();
    k_msleep(200);

    // Browser 1: six connections, one asset on each, then left open and
    // idle - exactly the state a browser sits in after the page has
    // loaded.
    int parked[kBrowserConnections];
    AssetFetch first[kBrowserConnections]{};
    size_t parkedCount = 0;
    for (size_t i = 0; i < kBrowserConnections; ++i) {
        parked[i] = tryConnectToServer();
        if (parked[i] >= 0) {
            ++parkedCount;
        }
        first[i].path = qcbridge::webassets::kAssets[i].path;
    }
    fetchAssetsInParallel(parked, first, kBrowserConnections);

    // Browser 1's live-update socket, upgraded and left open too.
    const int wsSock = tryConnectToServer();
    bool upgraded = false;
    if (wsSock >= 0 && sendAssetRequestRaw(wsSock, kWebsocketRequest)) {
        upgraded = contains(receiveAll(wsSock), "HTTP/1.1 101 Switching Protocols");
    }

    // Browser 2 arrives while every one of browser 1's connections is
    // still open. CONFIG_HTTP_SERVER_MAX_CLIENTS is 8, not 13 - sized to
    // what the board's TCP window budget can actually serve at once
    // (see ../../prj.conf's CONFIG_NET_TCP_MAX_SEND_WINDOW_SIZE comment)
    // rather than to two browsers' parked connections - so some of
    // these do have to wait for browser 1's idle slots to recycle. The
    // bar this test holds is therefore "served within one
    // CONFIG_HTTP_SERVER_CLIENT_INACTIVITY_TIMEOUT", not "served
    // immediately": a longer read timeout than the rest of the suite,
    // and still an order of magnitude inside the ~20-45s stalls the
    // owner saw on hardware when this was wrong.
    constexpr int kSecondBrowserTimeoutSeconds = 8;
    int second[kBrowserConnections];
    AssetFetch secondFetches[kBrowserConnections]{};
    size_t secondCount = 0;
    for (size_t i = 0; i < kBrowserConnections; ++i) {
        second[i] = tryConnectToServer(kSecondBrowserTimeoutSeconds);
        if (second[i] >= 0) {
            ++secondCount;
        }
        secondFetches[i].path = qcbridge::webassets::kAssets[i].path;
    }
    fetchAssetsInParallel(second, secondFetches, kBrowserConnections);

    for (size_t i = 0; i < kBrowserConnections; ++i) {
        if (parked[i] >= 0) {
            (void)zsock_close(parked[i]);
        }
        if (second[i] >= 0) {
            (void)zsock_close(second[i]);
        }
    }
    if (wsSock >= 0) {
        (void)zsock_close(wsSock);
    }

    zassert_equal(parkedCount, kBrowserConnections,
                  "first browser: only %zu of %zu connections were accepted", parkedCount,
                  kBrowserConnections);
    assertAllFetchesSucceeded(first, kBrowserConnections, "first browser's page load");
    zassert_true(upgraded, "first browser's websocket upgrade was refused");
    zassert_equal(secondCount, kBrowserConnections,
                  "second browser: only %zu of %zu connections were accepted", secondCount,
                  kBrowserConnections);
    assertAllFetchesSucceeded(secondFetches, kBrowserConnections, "second browser's page load");
}
