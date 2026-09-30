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

#pragma once

// WeeWX ObservingConditions catalog field declarations (open-astro#731),
// shared by weewx_schema.cpp (no vendor header) and weewx_catalog.cpp (the
// factory, vendor header allowed). No vendor header here either: the schema
// file includes this one and compiles in every build (including vendors-OFF).
// No field declares a required, min or max rule: the router arm this replaces
// refused a missing weewxUrl and a non-positive interval or timeout from both
// config sources, which a schema rule cannot do (Source::Persisted only warns
// and registers), so the factory refuses them with the arm's text instead. For
// the same reason the factory, not a max rule, refuses an interval or timeout
// above INT_MAX, the int domain the arm read them in.

#include <alpacacore/catalog/device_catalog.h>

#include <cstdint>
#include <string>
#include <vector>

// Designated initializers of Field<T> leave most members defaulted on purpose
// (matches AlpacaHTTP/tests/test_catalog_descriptor.h).
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace alpacacore::catalog {

// The WeeWX JSON feed URL (e.g. http://weewx.local/current.json).
inline const Field<std::string> kWeeWxUrl{.key = "weewxUrl", .default_value = ""};

inline const Field<std::int64_t> kWeeWxPollIntervalSeconds{.key = "pollIntervalSeconds", .default_value = 900};

inline const Field<std::int64_t> kWeeWxTimeoutMs{.key = "timeoutMs", .default_value = 5000};

inline const std::vector<FieldRef>& weewx_observingconditions_fields() {
    static const std::vector<FieldRef> fields{kWeeWxUrl.ref(), kWeeWxPollIntervalSeconds.ref(), kWeeWxTimeoutMs.ref()};
    return fields;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
