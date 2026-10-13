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

// Player One catalog field declarations (camera, Phoenix filter wheel, thermal
// switch), shared by playerone_schema.cpp (no vendor header) and
// playerone_catalog.cpp (the factories, vendor header allowed). No vendor
// header here either: the schema file includes this one and compiles in every
// build (including vendors-OFF).

#include <alpacacore/catalog/device_catalog.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// Designated initializers of Field<T> leave most members defaulted on purpose
// (matches AlpacaHTTP/tests/test_catalog_descriptor.h).
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace alpacacore::catalog {

// 0-based Player One SDK camera enumeration index; the thermal switch binds to
// a camera by it too. Range 0..INT_MAX so validation refuses a value the
// static_cast<int> in playerone_catalog.cpp would wrap; the driver still refuses
// an index past the attached cameras at connect.
inline const Field<std::int64_t> kPlayerOneCameraIndex{.key = "cameraIndex",
                                                       .default_value = 0,
                                                       .role = Role::EnumerationIndex,
                                                       .min = 0,
                                                       .max = std::numeric_limits<int>::max()};

// 0-based Phoenix wheel enumeration index, same range as above.
inline const Field<std::int64_t> kPlayerOneFilterwheelIndex{.key = "filterwheelIndex",
                                                            .default_value = 0,
                                                            .role = Role::EnumerationIndex,
                                                            .min = 0,
                                                            .max = std::numeric_limits<int>::max()};

// Slot names. Absent leaves the driver's own defaults ("Filter 1..N").
inline const Field<std::vector<std::string>> kPlayerOneFilterNames{.key = "filterNames", .default_value = {}};

inline const std::vector<FieldRef>& playerone_camera_fields() {
    static const std::vector<FieldRef> fields{kPlayerOneCameraIndex.ref()};
    return fields;
}

inline const std::vector<FieldRef>& playerone_filterwheel_fields() {
    static const std::vector<FieldRef> fields{kPlayerOneFilterwheelIndex.ref(), kPlayerOneFilterNames.ref()};
    return fields;
}

inline const std::vector<FieldRef>& playerone_switch_fields() {
    static const std::vector<FieldRef> fields{kPlayerOneCameraIndex.ref()};
    return fields;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
