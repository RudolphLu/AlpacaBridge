// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#include <alpacahttp/util/log_text.h>

#include <string>

#include "test_assert.h"

using alpacahttp::util::escape_for_log;

int main() {
    // Plain printable text and valid UTF-8 pass through.
    EXPECT(escape_for_log("/setup/v1/telescope/0/setup") == "/setup/v1/telescope/0/setup");
    EXPECT(escape_for_log("/caf\xC3\xA9") == "/caf\xC3\xA9");

    // Control bytes, DEL and NUL are escaped; a newline cannot forge a line.
    EXPECT(escape_for_log("a\x1b[31mb") == "a\\x1b[31mb");
    EXPECT(escape_for_log("a\nb\rc") == "a\\x0ab\\x0dc");
    EXPECT(escape_for_log("a\x7f"
                          "b\x08") == "a\\x7fb\\x08");
    EXPECT(escape_for_log(std::string("a\0b", 3)) == "a\\x00b");

    // Invalid UTF-8 is escaped byte by byte.
    EXPECT(escape_for_log("\xFF\xC3") == "\\xff\\xc3");
    EXPECT(escape_for_log("\x80") == "\\x80");
    EXPECT(escape_for_log("\xED\xA0\x80") == "\\xed\\xa0\\x80");  // surrogate

    // C1 controls and bidi controls are escaped byte by byte; range edges too.
    EXPECT(escape_for_log("a\xC2\x9B"
                          "b") == "a\\xc2\\x9bb");
    EXPECT(escape_for_log("\xE2\x80\xAE") == "\\xe2\\x80\\xae");
    EXPECT(escape_for_log("\xC2\x80\xC2\x9F") == "\\xc2\\x80\\xc2\\x9f");
    EXPECT(escape_for_log("\xE2\x80\xAA") == "\\xe2\\x80\\xaa");
    EXPECT(escape_for_log("\xE2\x81\xA6\xE2\x81\xA9") == "\\xe2\\x81\\xa6\\xe2\\x81\\xa9");
    // Neighbours pass through: U+00A0, U+2029, U+202F, U+2065, U+206A.
    EXPECT(escape_for_log("\xC2\xA0") == "\xC2\xA0");
    EXPECT(escape_for_log("\xE2\x80\xA9") == "\xE2\x80\xA9");
    EXPECT(escape_for_log("\xE2\x80\xAF") == "\xE2\x80\xAF");
    EXPECT(escape_for_log("\xE2\x81\xA5") == "\xE2\x81\xA5");
    EXPECT(escape_for_log("\xE2\x81\xAA") == "\xE2\x81\xAA");
    // Ordinary non-ASCII text is unchanged.
    EXPECT(escape_for_log("\xC3\xA9\xE4\xB8\xAD\xE6\x96\x87") == "\xC3\xA9\xE4\xB8\xAD\xE6\x96\x87");
    // A cut never splits an escaped sequence: the 3-byte control would end at 257.
    {  // case: cut does not split an escaped bidi control
        const std::string in = std::string(255, 'a') + "\xE2\x80\xAE";
        EXPECT(escape_for_log(in) == std::string(255, 'a') + "... (258 bytes)");
    }

    // Cut on a code-point boundary: 255 ASCII bytes then a 2-byte character
    // that would end at byte 257.
    {
        const std::string in = std::string(255, 'a') + "\xC3\xA9" + "tail";
        const std::string out = escape_for_log(in);
        EXPECT(out == std::string(255, 'a') + "... (" + std::to_string(in.size()) + " bytes)");
    }
    // A character ending exactly at the limit is kept.
    {
        const std::string in = std::string(254, 'a') + "\xC3\xA9" + "x";
        EXPECT(escape_for_log(in) == std::string(254, 'a') + "\xC3\xA9... (257 bytes)");
    }
    // Exactly at the limit: no suffix.
    EXPECT(escape_for_log(std::string(256, 'a')) == std::string(256, 'a'));
    // Custom limit.
    EXPECT(escape_for_log("abcdef", 3) == "abc... (6 bytes)");
    return 0;
}
