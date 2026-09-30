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

// The WeeWX ObservingConditions schema (open-astro#731). No vendor header:
// compiles in every build (including vendors-OFF), so `available` in
// GET /management/v1/devicecatalog reflects only whether a factory is
// registered. No cross-field normalize and no per-field rule: the refusals
// live in the factory (see weewx_fields.h).

#include "../../catalog/builtin_descriptors.h"
#include "weewx_fields.h"

namespace alpacacore::catalog {

void register_weewx_schema(DeviceCatalog& catalog) {
    Schema schema;
    schema.key = DeviceKey{"weewx", DeviceType::ObservingConditions};
    schema.display_name = "WeeWX Observing Conditions";
    schema.build_option = "ALPACACORE_ENABLE_WEEWX";
    schema.fields = weewx_observingconditions_fields();
    catalog.add(std::move(schema));
}

}  // namespace alpacacore::catalog
