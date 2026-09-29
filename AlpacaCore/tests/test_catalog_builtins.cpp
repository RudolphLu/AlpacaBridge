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

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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
