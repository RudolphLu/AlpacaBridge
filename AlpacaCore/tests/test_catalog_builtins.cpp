// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

// The built-in descriptors (open-astro#664, Part D): what
// register_builtin_schemas() and register_builtin_factories() put into a
// catalog. The schema half compiles in every build, so the first two cases
// run vendors-off too; the factory-selection cases need the Astroasis driver.
// Fake-only: Astroasis has no fake, so factory selection is observed through
// the connect refusal each factory produces without hardware (the fixed-path
// factory names the node it failed to open; the by-index factory reports the
// USB scan). Neither opens a real focuser: the path does not exist and the
// index is far beyond any bus.

#include <alpacacore/catalog/builtin_catalog.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/serial_io.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "catch2_compat.h"

using namespace alpacacore;
using namespace alpacacore::catalog;

namespace {

const DeviceKey kAstroasisKey{"astroasis", DeviceType::Focuser};

const DescriptorView* find_view(const std::vector<DescriptorView>& views, const DeviceKey& key) {
    for (const DescriptorView& v : views) {
        if (v.key == key) return &v;
    }
    return nullptr;
}

const FieldRef* find_field(std::span<const FieldRef> fields, std::string_view key) {
    for (const FieldRef& f : fields) {
        if (key == f.key) return &f;
    }
    return nullptr;
}

DeviceCatalog builtin_catalog() {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    register_builtin_factories(catalog);
    return catalog;
}

}  // namespace

TEST_CASE("Builtin catalog - register_builtin_schemas describes the Astroasis focuser in every build",
          "[catalog][astroasis][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kAstroasisKey);
    REQUIRE(v != nullptr);
    CHECK(v->display_name == "Astroasis Oasis Focuser");
    CHECK(v->build_option == "ALPACACORE_ENABLE_ASTROASIS");
    CHECK_FALSE(v->available);  // schemas only: no factory has been registered yet
    REQUIRE(v->fields.size() == 2);
    CHECK(std::string_view(v->fields[0].key) == "hidPath");
    CHECK(std::string_view(v->fields[1].key) == "focuserIndex");

    const FieldRef* hid = find_field(v->fields, "hidPath");
    REQUIRE(hid != nullptr);
    CHECK(hid->kind == FieldRef::Kind::String);
    CHECK(hid->role == Role::PortPath);
    CHECK_FALSE(hid->required);
    CHECK_FALSE(hid->applies_when.has_value());
    CHECK(hid->allowed_values.empty());
    CHECK_FALSE(hid->min.has_value());
    CHECK_FALSE(hid->max.has_value());
    REQUIRE(std::holds_alternative<std::string>(hid->default_value));
    CHECK(std::get<std::string>(hid->default_value).empty());

    const FieldRef* index = find_field(v->fields, "focuserIndex");
    REQUIRE(index != nullptr);
    CHECK(index->kind == FieldRef::Kind::Int);
    CHECK(index->role == Role::EnumerationIndex);
    CHECK_FALSE(index->required);
    CHECK_FALSE(index->applies_when.has_value());
    CHECK(index->allowed_values.empty());
    CHECK_FALSE(index->min.has_value());  // ALP-271: no range declared, see astroasis_fields.h
    CHECK_FALSE(index->max.has_value());  // F2 pins no range for focuserIndex either
    REQUIRE(std::holds_alternative<std::int64_t>(index->default_value));
    CHECK(std::get<std::int64_t>(index->default_value) == 0);

    // No rule on focuserIndex: a negative index passes through unchanged from
    // both sources -- the arm it replaces never validated the index either.
    DeviceConfig negative;
    negative.set("focuserIndex", std::int64_t{-1});
    const auto negative_api = catalog.normalize(kAstroasisKey, negative, Source::Api);
    CHECK_FALSE(negative_api.rejection.has_value());
    const auto negative_persisted = catalog.normalize(kAstroasisKey, negative, Source::Persisted);
    CHECK(negative_persisted.warnings.empty());
    REQUIRE(negative_persisted.config.has("focuserIndex"));
    const ConfigValue* negative_value = negative_persisted.config.find_value("focuserIndex");
    REQUIRE(negative_value != nullptr);
    REQUIRE(std::holds_alternative<std::int64_t>(*negative_value));
    CHECK(std::get<std::int64_t>(*negative_value) == -1);

    // Both keys accepted together, and an empty config normalizes to itself.
    DeviceConfig both;
    both.set("hidPath", std::string{"/dev/hidraw3"});
    both.set("focuserIndex", std::int64_t{2});
    CHECK_FALSE(catalog.normalize(kAstroasisKey, both, Source::Api).rejection.has_value());
    CHECK_FALSE(catalog.normalize(kAstroasisKey, DeviceConfig{}, Source::Api).rejection.has_value());

    // Sanitize keeps both (neither is a secret) and drops an undeclared key.
    both.set("junk", true);
    const DeviceConfig sanitized = catalog.sanitize(kAstroasisKey, both);
    CHECK(sanitized.has("hidPath"));
    CHECK(sanitized.has("focuserIndex"));
    CHECK_FALSE(sanitized.has("junk"));
}

TEST_CASE("Builtin catalog - register_builtin_factories makes the Astroasis focuser available only when built",
          "[catalog][astroasis][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kAstroasisKey);
    REQUIRE(v != nullptr);
#ifdef ALPACACORE_ENABLE_ASTROASIS
    CHECK(v->available);
#else
    CHECK_FALSE(v->available);
    // The catalog's own refusal names the build option; the router turns it
    // into the "<Vendor> support not enabled" text F2 pins (open-astro#664 Part C).
    CHECK_THROWS_AS(catalog.create(kAstroasisKey, DeviceConfig{}, 0), std::runtime_error);
    try {
        (void)catalog.create(kAstroasisKey, DeviceConfig{}, 0);
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("ALPACACORE_ENABLE_ASTROASIS") != std::string::npos);
    }
#endif
}

#ifdef ALPACACORE_ENABLE_ASTROASIS

namespace {

// The connect refusal a driver built from `config` produces with no hardware.
std::string connect_refusal(const DeviceCatalog& catalog, const DeviceConfig& config, int device_number) {
    auto driver = catalog.create(kAstroasisKey, config, device_number);
    REQUIRE(driver != nullptr);
    CHECK(driver->get_device_type() == DeviceType::Focuser);
    CHECK(driver->get_device_number() == device_number);
    CHECK_FALSE(driver->get_connected());
    try {
        driver->set_connected(true);
    } catch (const AlpacaException& e) {
        CHECK_FALSE(driver->get_connected());
        return e.what();
    }
    FAIL("set_connected(true) must throw with no focuser attached");
    return "";
}

}  // namespace

TEST_CASE("Builtin catalog - the Astroasis factory picks the hidPath factory when hidPath is set",
          "[catalog][astroasis][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const std::string path = "/dev/hidraw-alp255-none";
    DeviceConfig config;
    config.set("hidPath", path);
    config.set("focuserIndex", std::int64_t{1000000});  // ignored: hidPath wins, as the arm did it
    const std::string refusal = connect_refusal(catalog, config, 7);
    INFO(refusal);
    CHECK(refusal.find(path) != std::string::npos);
    CHECK(refusal.find("detected on the USB bus") == std::string::npos);
    CHECK(refusal.find("out of range") == std::string::npos);
}

TEST_CASE("Builtin catalog - the Astroasis factory scans by index when hidPath is empty or absent",
          "[catalog][astroasis][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    for (const bool with_empty_path : {true, false}) {
        DeviceConfig config;
        if (with_empty_path) config.set("hidPath", std::string{});
        config.set("focuserIndex", std::int64_t{1000000});
        const std::string refusal = connect_refusal(catalog, config, 8);
        INFO(refusal);
        // No hardware: the scan finds nothing. A bus with a real focuser on it
        // answers the out-of-range index instead; both come from the scan and
        // neither opens a device.
        CHECK((refusal.find("No Astroasis Oasis Focuser detected on the USB bus") != std::string::npos ||
               refusal.find("Focuser index 1000000 out of range") != std::string::npos));
        CHECK(refusal.find("hidraw") == std::string::npos);
    }
}

#endif  // ALPACACORE_ENABLE_ASTROASIS

// ---------------------------------------------------------------------------
// WeeWX (open-astro#731): the second built-in descriptor, same split as
// Astroasis. The schema declares no required/min/max rule: the three refusals
// of the router arm it replaces ("... requires weewxUrl", "... must be greater
// than 0") come from the factory, so a persisted entry that breaks one is still
// not registered, as before the move. The factory also refuses a value above
// the int range the arm read both numbers in ("... must be at most 2147483647").

namespace {

const DeviceKey kWeeWxKey{"weewx", DeviceType::ObservingConditions};

}  // namespace

TEST_CASE("Builtin catalog - register_builtin_schemas describes the WeeWX observing conditions in every build",
          "[catalog][weewx][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kWeeWxKey);
    REQUIRE(v != nullptr);
    // The router's not-enabled and registered texts use the first word.
    CHECK(v->display_name.substr(0, 6) == "WeeWX ");
    CHECK(v->build_option == "ALPACACORE_ENABLE_WEEWX");
    CHECK_FALSE(v->available);  // schemas only: no factory has been registered yet
    REQUIRE(v->fields.size() == 3);
    CHECK(std::string_view(v->fields[0].key) == "weewxUrl");
    CHECK(std::string_view(v->fields[1].key) == "pollIntervalSeconds");
    CHECK(std::string_view(v->fields[2].key) == "timeoutMs");

    for (const FieldRef& f : v->fields) {
        INFO(f.key);
        CHECK(f.role == Role::Plain);
        CHECK_FALSE(f.required);
        CHECK_FALSE(f.applies_when.has_value());
        CHECK(f.allowed_values.empty());
        CHECK_FALSE(f.min.has_value());
        CHECK_FALSE(f.max.has_value());
    }
    const FieldRef* url = find_field(v->fields, "weewxUrl");
    REQUIRE(url != nullptr);
    CHECK(url->kind == FieldRef::Kind::String);
    REQUIRE(std::holds_alternative<std::string>(url->default_value));
    CHECK(std::get<std::string>(url->default_value).empty());
    const FieldRef* poll = find_field(v->fields, "pollIntervalSeconds");
    REQUIRE(poll != nullptr);
    CHECK(poll->kind == FieldRef::Kind::Int);
    REQUIRE(std::holds_alternative<std::int64_t>(poll->default_value));
    CHECK(std::get<std::int64_t>(poll->default_value) == 900);
    const FieldRef* timeout = find_field(v->fields, "timeoutMs");
    REQUIRE(timeout != nullptr);
    CHECK(timeout->kind == FieldRef::Kind::Int);
    REQUIRE(std::holds_alternative<std::int64_t>(timeout->default_value));
    CHECK(std::get<std::int64_t>(timeout->default_value) == 5000);

    // No rule in the schema: a missing URL and zero values pass normalize
    // unchanged from both sources (the factory refuses them instead).
    DeviceConfig zeros;
    zeros.set("pollIntervalSeconds", std::int64_t{0});
    zeros.set("timeoutMs", std::int64_t{0});
    CHECK_FALSE(catalog.normalize(kWeeWxKey, zeros, Source::Api).rejection.has_value());
    const auto zeros_persisted = catalog.normalize(kWeeWxKey, zeros, Source::Persisted);
    CHECK(zeros_persisted.warnings.empty());
    CHECK(zeros_persisted.config.has("pollIntervalSeconds"));
    CHECK(zeros_persisted.config.has("timeoutMs"));

    // Sanitize keeps all three (none is a secret) and drops an undeclared key.
    DeviceConfig all;
    all.set("weewxUrl", std::string{"http://weewx.test:8998/current.json"});
    all.set("pollIntervalSeconds", std::int64_t{300});
    all.set("timeoutMs", std::int64_t{2500});
    all.set("cameraIndex", std::int64_t{1});
    const DeviceConfig sanitized = catalog.sanitize(kWeeWxKey, all);
    CHECK(sanitized.has("weewxUrl"));
    CHECK(sanitized.has("pollIntervalSeconds"));
    CHECK(sanitized.has("timeoutMs"));
    CHECK_FALSE(sanitized.has("cameraIndex"));
}

TEST_CASE("Builtin catalog - register_builtin_factories makes the WeeWX observing conditions available only when built",
          "[catalog][weewx][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kWeeWxKey);
    REQUIRE(v != nullptr);
#ifdef ALPACACORE_ENABLE_WEEWX
    CHECK(v->available);
#else
    CHECK_FALSE(v->available);
    CHECK_THROWS_AS(catalog.create(kWeeWxKey, DeviceConfig{}, 0), std::runtime_error);
    try {
        (void)catalog.create(kWeeWxKey, DeviceConfig{}, 0);
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("ALPACACORE_ENABLE_WEEWX") != std::string::npos);
    }
#endif
}

#ifdef ALPACACORE_ENABLE_WEEWX

namespace {

// A loopback HTTP endpoint the WeeWX driver can be pointed at. Answering, it
// serves a minimal current-conditions payload to every request; silent, it
// accepts each connection and never answers, so the client's own timeout ends
// the request. `requests()` counts connections accepted.
class LoopbackWeeWx {
public:
    explicit LoopbackWeeWx(bool answer) : answer_(answer) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd_ >= 0);
        // Non-blocking, so a connection reset between poll() and accept()
        // makes accept() fail with EAGAIN instead of blocking serve() and the
        // destructor's join. An accepted fd does not inherit the flag on Linux.
        REQUIRE(alpacacore::util::set_nonblocking(listen_fd_));
        const int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        REQUIRE(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(listen_fd_, 8) == 0);
        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~LoopbackWeeWx() {
        stop_ = true;
        thread_.join();
        for (const int fd : held_) ::close(fd);
        ::close(listen_fd_);
    }
    LoopbackWeeWx(const LoopbackWeeWx&) = delete;
    LoopbackWeeWx& operator=(const LoopbackWeeWx&) = delete;

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/current.json"; }
    int requests() const { return requests_.load(); }

private:
    void serve() {
        while (!stop_) {
            pollfd pfd{listen_fd_, POLLIN, 0};
            if (::poll(&pfd, 1, 20) <= 0) continue;
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) continue;
            ++requests_;
            if (!answer_) {
                held_.push_back(fd);
                continue;
            }
            std::string request;
            char buf[1024];
            while (request.find("\r\n\r\n") == std::string::npos) {
                const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                request.append(buf, static_cast<std::size_t>(n));
            }
            const std::string body = R"({"lcd_datasheet":{"current":{"outTemp":{"value":50.0}}}})";
            const std::string reply =
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
            (void)::send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
            ::close(fd);
        }
    }

    bool answer_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<int> requests_{0};
    std::vector<int> held_;  // serve() thread only, until the join
    std::thread thread_;
};

std::string create_refusal(const DeviceCatalog& catalog, const DeviceConfig& config) {
    try {
        (void)catalog.create(kWeeWxKey, config, 0);
    } catch (const AlpacaException& e) {
        CHECK(e.error_code() == AlpacaError::InvalidValue);
        return e.what();
    }
    FAIL("the WeeWX factory must refuse this config");
    return "";
}

}  // namespace

TEST_CASE("Builtin catalog - the WeeWX factory refuses what the router arm refused, with its text",
          "[catalog][weewx][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    DeviceConfig no_url;
    CHECK(create_refusal(catalog, no_url) == "WeeWX observing conditions requires weewxUrl");
    DeviceConfig empty_url;
    empty_url.set("weewxUrl", std::string{});
    CHECK(create_refusal(catalog, empty_url) == "WeeWX observing conditions requires weewxUrl");

    DeviceConfig poll_zero;
    poll_zero.set("weewxUrl", std::string{"http://weewx.test:8998/current.json"});
    poll_zero.set("pollIntervalSeconds", std::int64_t{0});
    CHECK(create_refusal(catalog, poll_zero) == "pollIntervalSeconds must be greater than 0");

    DeviceConfig timeout_negative;
    timeout_negative.set("weewxUrl", std::string{"http://weewx.test:8998/current.json"});
    timeout_negative.set("timeoutMs", std::int64_t{-1});
    CHECK(create_refusal(catalog, timeout_negative) == "timeoutMs must be greater than 0");
}

TEST_CASE("Builtin catalog - the WeeWX factory refuses an interval or timeout above the int range",
          "[catalog][weewx][unit]") {
    // The deleted arm read both fields as int. The schema's Int is 64-bit, and
    // 1e10 s converted to the nanoseconds wait_for() uses overflows, so the poll
    // thread waited 0 ms and fetched back to back.
    const DeviceCatalog catalog = builtin_catalog();
    DeviceConfig poll_huge;
    poll_huge.set("weewxUrl", std::string{"http://weewx.test:8998/current.json"});
    poll_huge.set("pollIntervalSeconds", std::int64_t{10000000000});
    CHECK(create_refusal(catalog, poll_huge) == "pollIntervalSeconds must be at most 2147483647");

    DeviceConfig timeout_huge;
    timeout_huge.set("weewxUrl", std::string{"http://weewx.test:8998/current.json"});
    timeout_huge.set("timeoutMs", std::int64_t{10000000000});
    CHECK(create_refusal(catalog, timeout_huge) == "timeoutMs must be at most 2147483647");
}

TEST_CASE("Builtin catalog - the WeeWX factory passes weewxUrl and timeoutMs through", "[catalog][weewx][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const LoopbackWeeWx silent(false);
    DeviceConfig config;
    config.set("weewxUrl", silent.url());
    config.set("timeoutMs", std::int64_t{300});
    auto driver = catalog.create(kWeeWxKey, config, 5);
    REQUIRE(driver != nullptr);
    CHECK(driver->get_device_type() == DeviceType::ObservingConditions);
    CHECK(driver->get_device_number() == 5);

    // The connect reaches this endpoint (the URL) and gives up after about
    // 300 ms (the timeout); the 5000 ms default would still be waiting.
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(driver->set_connected(true), AlpacaException);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    // serve() counts after its own poll() wakes, which a starved runner can
    // schedule after the client has already given up.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (silent.requests() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(silent.requests() >= 1);
    CHECK(elapsed < std::chrono::milliseconds(3000));
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Builtin catalog - the WeeWX factory passes pollIntervalSeconds through", "[catalog][weewx][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const LoopbackWeeWx server(true);
    DeviceConfig config;
    config.set("weewxUrl", server.url());
    config.set("pollIntervalSeconds", std::int64_t{1});
    auto driver = catalog.create(kWeeWxKey, config, 6);
    REQUIRE(driver != nullptr);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // One fetch at connect, one as the poll thread starts, then one per
    // interval: a third request within 5 s means a 1 s interval, where the
    // 900 s default would leave the count at two.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (server.requests() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(server.requests() >= 3);
    driver->set_connected(false);
}

#endif  // ALPACACORE_ENABLE_WEEWX
