### Fixed
- **The "Allowed host names" field and the "Restrict Host names" toggle are always editable in the web UI**, AlpacaHTTP, issue #787. With `ALPACAHTTP_ALLOWED_HOSTS` or `ALPACAHTTP_HOST_CHECK` set, both were read-only and a save that changed them was refused, so the Host allowlist could not be set from the web UI. The server no longer reads either variable: `http.allowed_hosts` and `http.host_check_enabled` come from the config file only, which the web UI writes. A systemd drop-in that still sets one is ignored; remove it. `GET /management/v1/description` no longer carries `HostCheckEnabledFixedByEnvironment` or `AllowedHostsFixedByEnvironment`.

### Changed
- **`/deploy-test` no longer installs the `ALPACAHTTP_ALLOWED_HOSTS=.lan` systemd drop-in**, issue #787. Step 4 now removes the old `allowed-hosts.conf` if a rig has it and reloads systemd before the restart; `.lan` is allowed through the web UI field or `http.allowed_hosts`. The `/conformu` notes say the same.
