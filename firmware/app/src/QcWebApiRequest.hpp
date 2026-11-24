#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

#include <etl/string.h>

namespace qcbridge {

// Page -> Board messages of the web API contract (docs/api/, ADR 0012
// decision 9): a small hand-written extractor over one text frame, no JSON
// library and no allocation. It reads the top-level object's "t", "id",
// "index" and "generation", skips every other member (both sides ignore
// fields they don't know), and turns anything it can't use into a
// QcWebApiBadRequest - which the Board answers with a badRequest error.

// docs/api/README.md: a page -> Board frame is at most 256 bytes.
inline constexpr size_t kWebApiMaxRequestBytes = 256;

// A command id: 1-16 characters of [0-9A-Za-z_-], chosen by the page.
inline constexpr size_t kWebApiMaxCommandIdLength = 16;
using QcWebApiCommandId = etl::string<kWebApiMaxCommandIdLength>;

// The largest Preset index a switchPreset may carry: the maximum of
// presetIndex in docs/api/schema/server-message.schema.json, which the
// client schema's switchPreset.index refers to.
inline constexpr uint32_t kWebApiMaxPresetIndex = 65535;

struct QcWebApiResync {};

struct QcWebApiSwitchPreset {
    QcWebApiCommandId id;
    uint32_t index = 0;
    uint32_t generation = 0;
};

struct QcWebApiBadRequest {
    // Present only when the frame carried a valid id.
    std::optional<QcWebApiCommandId> id;
    // English detail for the error's "message" - a string literal.
    std::string_view reason;
};

using QcWebApiRequest = std::variant<QcWebApiResync, QcWebApiSwitchPreset, QcWebApiBadRequest>;

namespace detail {

// The subset of a JSON reader the extractor needs: it validates the whole
// grammar (RFC 8259) while walking it, but only hands back raw slices of
// the input - a string's contents between its quotes, or a number's text.
class QcJsonScanner {
public:
    explicit QcJsonScanner(std::string_view text) : text_(text) {}

    bool atEnd() {
        skipSpace();
        return pos_ == text_.size();
    }

    bool consume(char c) {
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool peek(char c) {
        skipSpace();
        return pos_ < text_.size() && text_[pos_] == c;
    }

    // A string token's raw contents (escapes left as written).
    std::optional<std::string_view> string() {
        if (!consume('"')) {
            return std::nullopt;
        }
        const size_t start = pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == '"') {
                return text_.substr(start, pos_++ - start);
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return std::nullopt;
            }
            if (c == '\\') {
                if (!escape()) {
                    return std::nullopt;
                }
                continue;
            }
            ++pos_;
        }
        return std::nullopt;
    }

    // A number token's text, e.g. "3", "-0.5e2".
    std::optional<std::string_view> number() {
        skipSpace();
        const size_t start = pos_;
        accept('-');
        if (!accept('0') && !digits()) {
            return std::nullopt;
        }
        if (accept('.') && !digits()) {
            return std::nullopt;
        }
        if (accept('e') || accept('E')) {
            if (!accept('+')) {
                accept('-');
            }
            if (!digits()) {
                return std::nullopt;
            }
        }
        return text_.substr(start, pos_ - start);
    }

    // Any value, discarded. `depth` bounds nesting so a hostile frame can't
    // exhaust the stack.
    bool skipValue(int depth = 0) {
        if (depth > kMaxDepth) {
            return false;
        }
        skipSpace();
        if (pos_ >= text_.size()) {
            return false;
        }
        switch (text_[pos_]) {
        case '"':
            return string().has_value();
        case '{':
            return skipContainer('{', '}', depth, /*isObject=*/true);
        case '[':
            return skipContainer('[', ']', depth, /*isObject=*/false);
        case 't':
            return literal("true");
        case 'f':
            return literal("false");
        case 'n':
            return literal("null");
        default:
            return number().has_value();
        }
    }

private:
    static constexpr int kMaxDepth = 8;

    void skipSpace() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool accept(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool digits() {
        const size_t start = pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            ++pos_;
        }
        return pos_ > start;
    }

    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) {
            return false;
        }
        pos_ += word.size();
        return true;
    }

    // At a backslash inside a string: steps over one valid escape sequence.
    bool escape() {
        ++pos_;
        if (pos_ >= text_.size()) {
            return false;
        }
        const char c = text_[pos_++];
        if (c == 'u') {
            for (int i = 0; i < 4; ++i, ++pos_) {
                if (pos_ >= text_.size() || !isHexDigit(text_[pos_])) {
                    return false;
                }
            }
            return true;
        }
        return std::string_view("\"\\/bfnrt").find(c) != std::string_view::npos;
    }

    static bool isHexDigit(char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    bool skipContainer(char open, char close, int depth, bool isObject) {
        consume(open);
        if (consume(close)) {
            return true;
        }
        do {
            if (isObject && (!string() || !consume(':'))) {
                return false;
            }
            if (!skipValue(depth + 1)) {
                return false;
            }
        } while (consume(','));
        return consume(close);
    }

    std::string_view text_;
    size_t pos_ = 0;
};

// One member the extractor wants: absent, present with a usable value, or
// present with a value of the wrong shape.
template <typename T>
struct QcJsonField {
    bool present = false;
    std::optional<T> value;
};

inline bool isCommandIdCharacter(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-';
}

inline std::optional<QcWebApiCommandId> toCommandId(std::string_view raw) {
    if (raw.empty() || raw.size() > kWebApiMaxCommandIdLength) {
        return std::nullopt;
    }
    for (const char c : raw) {
        if (!isCommandIdCharacter(c)) {
            return std::nullopt;
        }
    }
    return QcWebApiCommandId(raw.data(), raw.size());
}

// A plain non-negative integer up to `max` - "3", not "3.0", "-1" or "3e0".
inline std::optional<uint32_t> toUInt(std::string_view raw, uint32_t max) {
    if (raw.empty()) {
        return std::nullopt;
    }
    uint64_t value = 0;
    for (const char c : raw) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<uint64_t>(c - '0');
        if (value > max) {
            return std::nullopt;
        }
    }
    return static_cast<uint32_t>(value);
}

inline std::optional<std::string_view> toText(std::string_view raw) { return raw; }

// The value of a member whose key was just read, into `field`: a string
// through `convert` (which yields nothing for a string of the wrong shape),
// anything else marks the field present but unusable. False only when the
// value isn't valid JSON at all.
template <typename T, typename Convert>
bool readStringField(QcJsonScanner& scanner, QcJsonField<T>& field, Convert convert) {
    field.present = true;
    field.value.reset();
    if (!scanner.peek('"')) {
        return scanner.skipValue();
    }
    const auto raw = scanner.string();
    if (!raw) {
        return false;
    }
    field.value = convert(*raw);
    return true;
}

// Same, for a number that must be a plain integer from 0 to `max`.
inline bool readUIntField(QcJsonScanner& scanner, QcJsonField<uint32_t>& field, uint32_t max) {
    field.present = true;
    field.value.reset();
    if (scanner.peek('"')) {
        return scanner.skipValue();
    }
    const auto raw = scanner.number();
    if (!raw) {
        return scanner.skipValue();
    }
    field.value = toUInt(*raw, max);
    return true;
}

}  // namespace detail

// `text` is the frame as received; `frameLength` its full length, which is
// larger than text.size() when the transport had to cut an oversized frame
// short to read it.
inline QcWebApiRequest parseWebApiRequest(std::string_view text, size_t frameLength) {
    if (frameLength > kWebApiMaxRequestBytes) {
        return QcWebApiBadRequest{.id = std::nullopt, .reason = "frame too large"};
    }

    detail::QcJsonField<std::string_view> type;
    detail::QcJsonField<QcWebApiCommandId> id;
    detail::QcJsonField<uint32_t> index;
    detail::QcJsonField<uint32_t> generation;

    detail::QcJsonScanner scanner(text);
    const auto unparseable = QcWebApiBadRequest{.id = std::nullopt, .reason = "unparseable"};
    if (!scanner.peek('{')) {
        return scanner.skipValue() && scanner.atEnd()
                   ? QcWebApiBadRequest{.id = std::nullopt, .reason = "not an object"}
                   : unparseable;
    }
    scanner.consume('{');
    if (!scanner.consume('}')) {
        do {
            const auto key = scanner.string();
            if (!key || !scanner.consume(':')) {
                return unparseable;
            }
            bool ok = false;
            if (*key == "t") {
                ok = detail::readStringField(scanner, type, detail::toText);
            } else if (*key == "id") {
                ok = detail::readStringField(scanner, id, detail::toCommandId);
            } else if (*key == "index") {
                ok = detail::readUIntField(scanner, index, kWebApiMaxPresetIndex);
            } else if (*key == "generation") {
                ok = detail::readUIntField(scanner, generation, UINT32_MAX);
            } else {
                ok = scanner.skipValue();
            }
            if (!ok) {
                return unparseable;
            }
        } while (scanner.consume(','));
        if (!scanner.consume('}')) {
            return unparseable;
        }
    }
    if (!scanner.atEnd()) {
        return unparseable;
    }

    if (!type.value) {
        return QcWebApiBadRequest{.id = id.value, .reason = "missing t"};
    }
    if (*type.value == "resync") {
        return QcWebApiResync{};
    }
    if (*type.value != "switchPreset") {
        return QcWebApiBadRequest{.id = id.value, .reason = "unknown type"};
    }
    if (!id.value) {
        return QcWebApiBadRequest{.id = std::nullopt, .reason = "bad id"};
    }
    if (!index.value) {
        return QcWebApiBadRequest{.id = id.value, .reason = "bad index"};
    }
    if (!generation.value) {
        return QcWebApiBadRequest{.id = id.value, .reason = "bad generation"};
    }
    return QcWebApiSwitchPreset{.id = *id.value, .index = *index.value, .generation = *generation.value};
}

}  // namespace qcbridge
