### Fixed

- **A Player One camera or filter wheel index outside 0..2147483647 is refused by catalog validation** (AlpacaCore, issue #1058): `cameraIndex` and `filterwheelIndex` declare that range, so a value of 2^31 or above no longer wraps in the `static_cast<int>` in `playerone_catalog.cpp`; covered by "Builtin catalog - Player One index range refuses values the int cast would wrap". A saved entry already outside that range is reset to index 0 at start-up ("config normalized" in the log), so check the index of a Player One device that never connected.
