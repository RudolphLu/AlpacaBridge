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

// Built-in descriptor composition (open-astro#710). Defined in the
// alpacacore_builtins library, not in alpacacore: the core stays vendor-free,
// and only a binary that links alpacacore_builtins (the server) knows which
// vendor descriptors this build has.

#include <alpacacore/catalog/device_catalog.h>

namespace alpacacore::catalog {

// Register every vendor descriptor slice landed so far (Astroasis today); a factory is
// registered only when its vendor is built. Pattern: docs/decisions/0004-device-catalog.md.
void register_builtin_schemas(DeviceCatalog& catalog);
void register_builtin_factories(DeviceCatalog& catalog);

}  // namespace alpacacore::catalog
