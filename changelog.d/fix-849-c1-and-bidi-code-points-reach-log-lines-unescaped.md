### Fixed

- **AlpacaHTTP: C1 and bidi control characters in log text are escaped** (issue #849). `util::escape_for_log` let U+0080..U+009F (U+009B is CSI on some terminals) and the bidi controls U+202A..U+202E and U+2066..U+2069 through, so a client-supplied value could drive a terminal or reorder a displayed log line. They are now written as `\xNN` per UTF-8 byte, like other control characters; the `max_bytes` cut still never splits one (`AlpacaHTTP/tests/test_log_text.cpp`).
