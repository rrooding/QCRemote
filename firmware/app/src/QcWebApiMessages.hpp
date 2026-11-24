#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include <etl/vector.h>

#include "QcCatalogJson.hpp"
#include "QcPresetTransport.hpp"

namespace qcbridge {

// Board -> page message encoders for the web API contract (docs/api/,
// ADR 0012) - hand-written, over caller-provided fixed-capacity storage,
// with the same detail::append* helpers as QcCatalogJson.hpp. Their output
// is compared byte for byte with docs/api/examples/server in
// tests/web_api_session. QcWebApiSession.hpp decides when each is sent and
// with which seq; these only format.

enum class QcWebApiErrorCode { QcDisconnected, BadIndex, StaleCatalog, Busy, BadRequest };

namespace detail {

inline std::string_view toString(QcWebApiErrorCode code) {
    switch (code) {
    case QcWebApiErrorCode::QcDisconnected:
        return "qcDisconnected";
    case QcWebApiErrorCode::BadIndex:
        return "badIndex";
    case QcWebApiErrorCode::StaleCatalog:
        return "staleCatalog";
    case QcWebApiErrorCode::Busy:
        return "busy";
    case QcWebApiErrorCode::BadRequest:
        break;
    }
    return "badRequest";
}

template <size_t N>
inline void appendMessageStart(std::string_view type, uint32_t seq, etl::vector<char, N>& out) {
    out.clear();
    appendLiteral("{\"t\":\"", out);
    appendLiteral(type, out);
    appendLiteral("\",\"seq\":", out);
    appendUInt(seq, out);
}

template <size_t N>
inline void appendCurrentSection(const QcCurrentPreset& current, etl::vector<char, N>& out) {
    appendLiteral("\"current\":{\"index\":", out);
    if (current) {
        appendUInt(current->index, out);
        appendLiteral(",\"name\":", out);
        appendQuoted(std::string_view(current->name.data(), current->name.size()), out);
    } else {
        appendLiteral("null,\"name\":null", out);
    }
    appendLiteral(",\"scene\":null}", out);
}

template <size_t N>
inline void appendLinkSection(const QcLinkState& link, etl::vector<char, N>& out) {
    appendLiteral("\"link\":{\"qcConnected\":", out);
    appendLiteral(link.qcConnected ? "true" : "false", out);
    appendLiteral("}", out);
}

}  // namespace detail

// {"t":"snapshot","seq":0,"build":...,"catalog":...,"current":...,"link":...}
// A snapshot's seq is always 0. `catalogJson` is encodeCatalogJson()'s output.
template <size_t N>
inline void encodeSnapshotMessage(std::string_view build, std::string_view catalogJson,
                                  const QcCurrentPreset& current, const QcLinkState& link,
                                  etl::vector<char, N>& out) {
    detail::appendMessageStart("snapshot", 0, out);
    detail::appendLiteral(",\"build\":", out);
    detail::appendQuoted(build, out);
    detail::appendLiteral(",\"catalog\":", out);
    detail::appendLiteral(catalogJson, out);
    detail::appendLiteral(",", out);
    detail::appendCurrentSection(current, out);
    detail::appendLiteral(",", out);
    detail::appendLinkSection(link, out);
    detail::appendLiteral("}", out);
}

// {"t":"patch","seq":N,"current":{...}}
template <size_t N>
inline void encodeCurrentPatchMessage(uint32_t seq, const QcCurrentPreset& current, etl::vector<char, N>& out) {
    detail::appendMessageStart("patch", seq, out);
    detail::appendLiteral(",", out);
    detail::appendCurrentSection(current, out);
    detail::appendLiteral("}", out);
}

// {"t":"patch","seq":N,"link":{...}}
template <size_t N>
inline void encodeLinkPatchMessage(uint32_t seq, const QcLinkState& link, etl::vector<char, N>& out) {
    detail::appendMessageStart("patch", seq, out);
    detail::appendLiteral(",", out);
    detail::appendLinkSection(link, out);
    detail::appendLiteral("}", out);
}

// {"t":"tick","seq":N}
template <size_t N>
inline void encodeTickMessage(uint32_t seq, etl::vector<char, N>& out) {
    detail::appendMessageStart("tick", seq, out);
    detail::appendLiteral("}", out);
}

// {"t":"error","seq":N,"id":"...","code":"...","message":"..."} - "id" only
// when the rejected command's id could be read.
template <size_t N>
inline void encodeErrorMessage(uint32_t seq, std::optional<std::string_view> id, QcWebApiErrorCode code,
                               std::string_view message, etl::vector<char, N>& out) {
    detail::appendMessageStart("error", seq, out);
    if (id) {
        detail::appendLiteral(",\"id\":", out);
        detail::appendQuoted(*id, out);
    }
    detail::appendLiteral(",\"code\":\"", out);
    detail::appendLiteral(detail::toString(code), out);
    detail::appendLiteral("\",\"message\":", out);
    detail::appendQuoted(message, out);
    detail::appendLiteral("}", out);
}

}  // namespace qcbridge
