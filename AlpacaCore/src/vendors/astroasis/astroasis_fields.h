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

// Astroasis Oasis Focuser catalog field declarations (open-astro#664), shared
// by astroasis_schema.cpp (no vendor header) and astroasis_catalog.cpp (the
// factory, vendor header allowed). No vendor header here either: the schema
// file includes this one and compiles in every build (including vendors-OFF),
// and the layering gate scans only the schema file, not its includes.

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

// Explicit hidapi device path (e.g. from enumerate_astroasis_focusers()).
// Takes precedence over kFocuserIndex when both are present -- the factory
// checks hidPath.empty() first, same as the arm it replaces.
inline const Field<std::string> kAstroasisHidPath{.key = "hidPath", .default_value = "", .role = Role::PortPath};

// 0-based index into the USB HID bus scan (enumerate_astroasis_focusers()),
// used only when hidPath is absent or empty. No declared range (ALP-271,
// deviation from open-astro#664's "min 0" line, Diego's decision ALP-268):
// #664 says a descriptor may not declare a range an F2 case does not already
// pin, and F2 pins nothing for focuserIndex; a negative value now passes
// through unvalidated exactly as the arm it replaces did, and the driver
// itself still refuses an out-of-range index at connect.
inline const Field<std::int64_t> kAstroasisFocuserIndex{
    .key = "focuserIndex", .default_value = 0, .role = Role::EnumerationIndex};

inline const std::vector<FieldRef>& astroasis_focuser_fields() {
    static const std::vector<FieldRef> fields{kAstroasisHidPath.ref(), kAstroasisFocuserIndex.ref()};
    return fields;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
