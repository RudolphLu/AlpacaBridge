### Fixed

- Sky-Watcher: the pulse-guide ownership deadline now runs on the injected task clock like the pulse hold and every other driver deadline, instead of the host steady clock (issue #994). Production timing is unchanged; covered by `SkyWatcher async - IsPulseGuiding follows the task clock, not the host clock (#994)`.
