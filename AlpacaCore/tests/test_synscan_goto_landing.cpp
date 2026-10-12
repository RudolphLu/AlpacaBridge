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

// open-astro#880, SynScan: the driver reports the handset's own position after
// a GOTO, and a handset that lands ~29" off in RA (EQM-35 Pro, 1.96 s of RA)
// fails ConformU's +/-10" SlewToCoordinates check. The driver re-issues the
// GOTO from the handset's own readback until the landing is inside tolerance.
// The fake lands every GOTO a fixed RA offset away from what it was told, re-rolls that by one second of RA on
// every other GOTO when `reroll` is set (as the handset does: two GOTOs with an identical aim landed 194 counts
// apart), and executes the driver's timed guide-rate moves (`P` variable rate, then stop) exactly, adding the
// tracking gap an RA stop/restart costs on the handset (open-astro#1030).
//
// open-astro#1030: the handset advances its local sidereal time once a second, so its RA readback after a GOTO is
// a sawtooth, exact right after the tick and one second of RA (15") low just before the next. With `sawtooth` set
// the fake reports RA that way, from its own epoch, so a test can sample the driver at a chosen phase.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "fake_mount_server.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kCountsMask = 0xFFFFFF;
// 1.96 s of RA (the offset measured on the EQM-35 Pro) in 24-bit counts: 1.96 / 86400 * 2^24.
constexpr uint32_t kRaOffsetCounts = 381;
// One second of RA in 24-bit counts: 2^24 / 86400.
constexpr double kCountsPerSecondOfRa = 194.18;
constexpr double kCountsPerArcsec = 1.0 / 0.0772;
// The tracking gap an RA stop/restart costs on the handset: measured 3.5" extra on a 15" east pulse.
constexpr int32_t kRaStopGapCounts = 45;

struct LandingOffsetHandset {
    std::atomic<uint32_t> ra_raw{0x400000};
    std::atomic<uint32_t> dec_raw{0x100000};
    std::atomic<int> goto_count{0};
    std::atomic<Clock::rep> goto_started{0};
    std::atomic<Clock::rep> first_goto_at{0};
    std::atomic<Clock::rep> second_goto_at{0};
    std::atomic<bool> goto_stopped{false};
    // Each GOTO lands this many counts past the commanded RA; multiplied by the GOTO ordinal when `growing`.
    std::atomic<uint32_t> ra_offset_counts{kRaOffsetCounts};
    std::atomic<bool> growing{false};
    std::atomic<int> goto_ms{300};
    // When set, GOTO n (1-based, up to the array size) lands dec_offset_script[n-1] counts off the commanded Dec:
    // the random per-GOTO Dec scatter a SynScan handset shows on a real mount (#1027).
    std::atomic<bool> dec_scripted{false};
    std::array<std::atomic<int32_t>, 4> dec_offset_script{};
    // The once-a-second RA readback sawtooth (#1030), phased from `sawtooth_epoch`.
    std::atomic<bool> sawtooth{false};
    std::atomic<Clock::rep> sawtooth_epoch{0};
    // Every other GOTO lands one further second of RA east (#1030).
    std::atomic<bool> reroll{false};
    // A GOTO asked to move Dec by 16" or less lands this many counts off, alternating sign (backlash-shaped).
    std::atomic<int32_t> dec_short_move_scatter{0};
    // Timed moves: `P` variable-rate start records the axis velocity; the stop applies the displacement.
    std::atomic<bool> ignore_nudges{false};
    std::atomic<int> nudge_starts{0};
    std::atomic<int> nudge_count{0};  // stops received
    std::atomic<Clock::rep> last_nudge_stop_at{0};
    std::array<std::atomic<double>, 2> axis_counts_per_s{};
    std::array<std::atomic<Clock::rep>, 2> axis_move_started{};

    double sawtooth_phase() const {
        const auto epoch = Clock::time_point(Clock::duration(sawtooth_epoch.load()));
        const double since = std::chrono::duration<double>(Clock::now() - epoch).count();
        return since - std::floor(since);
    }

    bool goto_in_progress() const {
        if (goto_count.load() == 0) return false;
        const auto started = Clock::time_point(Clock::duration(goto_started.load()));
        return !goto_stopped.load() && Clock::now() - started < std::chrono::milliseconds(goto_ms.load());
    }
};

std::string hex24(uint32_t raw) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06X00", raw & kCountsMask);
    return buf;
}

alpacacore::test::FakeMountServer::Responder responder(const std::shared_ptr<LandingOffsetHandset>& st) {
    return [st](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        switch (chunk[0]) {
            case 'K':
                return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
            case 'e': {
                uint32_t ra = st->ra_raw.load();
                if (st->sawtooth.load()) {
                    const auto low = static_cast<uint32_t>(std::lround(st->sawtooth_phase() * kCountsPerSecondOfRa));
                    ra = (ra + kCountsMask + 1 - low) & kCountsMask;
                }
                return hex24(ra) + "," + hex24(st->dec_raw.load()) + "#";
            }
            case 'r': {
                // "r" + 6 hex + "00" + "," + 6 hex + "00"
                if (chunk.size() < 18) return "";
                const uint32_t ra = static_cast<uint32_t>(std::strtoul(chunk.substr(1, 6).c_str(), nullptr, 16));
                const uint32_t dec = static_cast<uint32_t>(std::strtoul(chunk.substr(10, 6).c_str(), nullptr, 16));
                const int ordinal = st->goto_count.fetch_add(1) + 1;
                uint32_t offset = st->ra_offset_counts.load() * (st->growing.load() ? ordinal : 1U);
                if (st->reroll.load() && ordinal % 2 == 0) {
                    offset += static_cast<uint32_t>(std::lround(kCountsPerSecondOfRa));
                }
                const int32_t requested_dec_move = static_cast<int32_t>(dec) - static_cast<int32_t>(st->dec_raw.load());
                st->ra_raw.store((ra + offset) & kCountsMask);
                int32_t dec_offset = 0;
                if (st->dec_scripted.load() && ordinal <= static_cast<int>(st->dec_offset_script.size())) {
                    dec_offset = st->dec_offset_script[static_cast<std::size_t>(ordinal - 1)].load();
                }
                if (st->dec_short_move_scatter.load() != 0 && std::abs(requested_dec_move) <= 210) {
                    dec_offset +=
                        ordinal % 2 == 0 ? st->dec_short_move_scatter.load() : -st->dec_short_move_scatter.load();
                }
                st->dec_raw.store(static_cast<uint32_t>(static_cast<int32_t>(dec) + dec_offset) & kCountsMask);
                st->goto_started.store(Clock::now().time_since_epoch().count());
                if (ordinal == 1) st->first_goto_at.store(st->goto_started.load());
                if (ordinal == 2) st->second_goto_at.store(st->goto_started.load());
                st->goto_stopped.store(false);
                return "#";
            }
            case 'L':
                return st->goto_in_progress() ? "1#" : "0#";
            case 'M':
                st->goto_stopped.store(true);
                return "#";
            case 'P': {
                if (chunk.size() >= 8 && chunk[1] == 3 && chunk[2] == 16 && chunk[4] == 0 && chunk[5] == 0) {
                    st->goto_stopped.store(true);
                }
                if (chunk.size() >= 8 && chunk[1] == 3 && (chunk[2] == 16 || chunk[2] == 17)) {
                    const std::size_t axis = chunk[2] == 16 ? 0 : 1;
                    const double rate_arcsec =
                        ((static_cast<unsigned char>(chunk[4]) << 8) | static_cast<unsigned char>(chunk[5])) / 4.0;
                    if (rate_arcsec == 0.0) {
                        const double counts_per_s = st->axis_counts_per_s[axis].exchange(0.0);
                        if (counts_per_s != 0.0) {
                            const auto started = Clock::time_point(Clock::duration(st->axis_move_started[axis].load()));
                            const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
                            int32_t moved = static_cast<int32_t>(std::lround(counts_per_s * elapsed));
                            if (axis == 0) moved += kRaStopGapCounts;
                            if (!st->ignore_nudges.load()) {
                                auto& raw = axis == 0 ? st->ra_raw : st->dec_raw;
                                raw.store(static_cast<uint32_t>(static_cast<int32_t>(raw.load()) + moved) &
                                          kCountsMask);
                            }
                            st->nudge_count.fetch_add(1);
                            st->last_nudge_stop_at.store(Clock::now().time_since_epoch().count());
                        }
                    } else {
                        // RA: code 7 (the negative variable rate) moves east, raising RA; Dec: code 6 raises Dec.
                        const bool positive = axis == 0 ? chunk[3] == 7 : chunk[3] == 6;
                        st->axis_counts_per_s[axis].store((positive ? 1.0 : -1.0) * rate_arcsec * kCountsPerArcsec);
                        st->axis_move_started[axis].store(Clock::now().time_since_epoch().count());
                        st->nudge_starts.fetch_add(1);
                    }
                }
                return "#";
            }
            case 'T':
                return "#";
            case 'm':
                return std::string(1, static_cast<char>(50)) + "#";
            case 't':  // tracking mode read: sidereal tracking on
                return std::string(1, '\x01') + "#";
            case 'w':
                return std::string(8, '\0') + "#";
            case 'W':
                return "#";
            default:
                return "0#";
        }
    };
}

// A 15"/s guide rate (the driver's 1x sidereal ceiling) halves the timed landing moves in tests.
void use_fast_guide_rate(alpacacore::TelescopeDriver& driver) {
    driver.set_guide_rate(alpacacore::GuideRate{15.0 / 3600.0, 15.0 / 3600.0});
}

alpacacore::vendor::synscan::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 1000;
    return info;
}

double ra_error_arcsec(alpacacore::TelescopeDriver& driver, double target_hours) {
    return std::abs(driver.get_right_ascension() - target_hours) * 15.0 * 3600.0;
}

double dec_error_arcsec(alpacacore::TelescopeDriver& driver, double target_degrees) {
    return std::abs(driver.get_declination() - target_degrees) * 3600.0;
}

constexpr double kTargetRa = 5.5;
constexpr double kTargetDec = 20.0;
// ConformU's SlewToCoordinates tolerance.
constexpr double kConformUToleranceArcsec = 10.0;

// The EQM-35 Pro on its HC 06.03.00 handset (#1027): a GOTO lands RA +10.4" east (systematic) and Dec up to
// 10" off at random; a GOTO asked to move Dec by 16" or less lands worse (7.3" mean, 19.4" max on 2026-10-13),
// so a Dec residual is never chased with another GOTO.
void script_rig_dec_scatter(LandingOffsetHandset& st) {
    st.ra_offset_counts.store(135);  // 10.4"
    const std::array<int32_t, 4> dec_counts{124, 1, -139, -10};  // the first GOTO lands Dec 9.6" off
    st.dec_short_move_scatter.store(150);
    for (std::size_t i = 0; i < dec_counts.size(); ++i) {
        st.dec_offset_script[i].store(dec_counts[i]);
    }
    st.dec_scripted.store(true);
}

// Blocks until the fake's sawtooth is at `phase` (0..1) of its second, within 20 ms.
void wait_for_sawtooth_phase(const LandingOffsetHandset& st, double phase) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (std::abs(st.sawtooth_phase() - phase) > 0.02 && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Counts the SynScan WARN that reports a landing still off after the last refinement pass. The sink may be
// called from the async slew thread, so the count is atomic.
struct LandingWarnCounter {
    std::atomic<int> warns{0};
    alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
    LandingWarnCounter() {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
                if (level == alpacacore::logging::LogLevel::Warn && component == "SynScan" &&
                    message.find("refinement passes") != std::string_view::npos) {
                    warns.fetch_add(1);
                }
            });
    }
    ~LandingWarnCounter() { alpacacore::logging::set_log_sink(previous); }
    LandingWarnCounter(const LandingWarnCounter&) = delete;
    LandingWarnCounter& operator=(const LandingWarnCounter&) = delete;
};

}  // namespace

TEST_CASE(
    "SynScan GOTO landing - SlewToCoordinates finishes a GOTO the handset lands 29 arcsec off with a timed move (#880)",
    "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));

    CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
    CHECK(dec_error_arcsec(*driver, kTargetDec) < 3.0);
    CHECK(st->goto_count.load() == 1);  // never another GOTO: the handset re-rolls each landing by a second
    CHECK(st->nudge_count.load() >= 1);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - the settle time follows the last landing move once (#880)",
          "[synscan][telescope][goto-landing]") {
    struct Timing {
        Clock::duration until_last_move;
        Clock::duration after_last_move;
    };
    // One SlewToCoordinates that needs a landing move, with the given settle time.
    auto run = [](int settle_seconds) {
        auto st = std::make_shared<LandingOffsetHandset>();
        alpacacore::test::FakeMountServer server(responder(st));
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
            0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        use_fast_guide_rate(*driver);
        driver->set_slew_settle_time(settle_seconds);
        REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));
        const auto returned = Clock::now();
        REQUIRE(st->goto_count.load() == 1);
        REQUIRE(st->nudge_count.load() >= 1);
        const auto first = Clock::time_point(Clock::duration(st->first_goto_at.load()));
        const auto last_move = Clock::time_point(Clock::duration(st->last_nudge_stop_at.load()));
        driver->set_connected(false);
        return Timing{last_move - first, returned - last_move};
    };
    constexpr int kSettleSeconds = 3;
    const Timing none = run(0);
    const Timing settled = run(kSettleSeconds);
    const auto slack = std::chrono::milliseconds(1500);

    // The landing moves go out before any settle wait: no settle inside the landing...
    CHECK(settled.until_last_move < none.until_last_move + slack);
    // ...and exactly one settle period follows the last move (none: dropped; twice: doubled).
    CHECK(settled.after_last_move >= none.after_last_move + std::chrono::seconds(kSettleSeconds) - slack);
    CHECK(settled.after_last_move < none.after_last_move + std::chrono::seconds(kSettleSeconds) + slack);
}

TEST_CASE("SynScan GOTO landing - SlewToTarget refines the landing too (#880)", "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->set_target_right_ascension(kTargetRa);
    driver->set_target_declination(kTargetDec);
    REQUIRE_NOTHROW(driver->slew_to_target());

    CHECK(ra_error_arcsec(*driver, kTargetRa) < kConformUToleranceArcsec);
    CHECK(st->goto_count.load() == 1);
    CHECK(st->nudge_count.load() >= 1);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - async slew keeps Slewing true until the refinement lands (#880)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + std::chrono::seconds(40);
    while (driver->get_slewing() && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE_FALSE(driver->get_slewing());

    // Slewing fell only after the landing moves, so the landing is final.
    CHECK(st->goto_count.load() == 1);
    CHECK(st->nudge_count.load() >= 1);
    CHECK(ra_error_arcsec(*driver, kTargetRa) < kConformUToleranceArcsec);
    CHECK(dec_error_arcsec(*driver, kTargetDec) < kConformUToleranceArcsec);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a landing that never converges is bounded and reported (#880)",
          "[synscan][telescope][goto-landing]") {
    LandingWarnCounter counter;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ignore_nudges.store(true);  // the timed moves never take: the residual stays
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    // Reported as a WARN and the slew completes (#1027): the residual is the handset's, not a failed command.
    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));
    CHECK(counter.warns.load() == 1);
    CHECK(st->goto_count.load() == 1);
    CHECK(st->nudge_count.load() == 3);  // one move per round, three rounds, no more
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - an async landing that never converges is bounded and reported (#880)",
          "[synscan][telescope][goto-landing]") {
    LandingWarnCounter counter;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ignore_nudges.store(true);  // the timed moves never take: the residual stays
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    bool slewing = true;
    while (slewing && Clock::now() < deadline) {
        // Reported as a WARN, not as a slew error on the next Slewing read (#1027).
        REQUIRE_NOTHROW(slewing = driver->get_slewing());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK_FALSE(slewing);
    CHECK(counter.warns.load() == 1);
    CHECK(st->goto_count.load() == 1);
    CHECK(st->nudge_count.load() == 3);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - Dec scatter of the GOTO is removed by a timed move, never another GOTO (#1027)",
          "[synscan][telescope][goto-landing]") {
    LandingWarnCounter counter;
    auto st = std::make_shared<LandingOffsetHandset>();
    script_rig_dec_scatter(*st);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));

    CHECK(st->goto_count.load() == 1);  // a short Dec re-GOTO would land anywhere within 16"
    CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
    CHECK(dec_error_arcsec(*driver, kTargetDec) < 3.0);
    CHECK(counter.warns.load() == 0);
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - async Dec scatter of the GOTO is removed by a timed move, never another GOTO (#1027)",
          "[synscan][telescope][goto-landing]") {
    LandingWarnCounter counter;
    auto st = std::make_shared<LandingOffsetHandset>();
    script_rig_dec_scatter(*st);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    bool slewing = true;
    while (slewing && Clock::now() < deadline) {
        REQUIRE_NOTHROW(slewing = driver->get_slewing());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK_FALSE(slewing);
    CHECK(st->goto_count.load() == 1);
    CHECK(dec_error_arcsec(*driver, kTargetDec) < 3.0);
    CHECK(counter.warns.load() == 0);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - RightAscension is corrected for the handset's one-second readback sawtooth (#1030)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ra_offset_counts.store(165);  // 12.7": the EQM-35 Pro's per-GOTO east landing offset, read at the tick
    st->sawtooth_epoch.store(Clock::now().time_since_epoch().count());
    st->sawtooth.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    use_fast_guide_rate(*driver);
    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));
    CHECK(st->goto_count.load() == 1);

    // Each sample is a fresh handset read (the position cache lasts 2 s) at a chosen phase of the handset's second;
    // uncorrected, the phase-0.9 read is 13.5" low and the three spread by 11".
    for (const double phase : {0.15, 0.5, 0.9}) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        wait_for_sawtooth_phase(*st, phase);
        INFO("phase " << phase);
        CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
    }
    CHECK(dec_error_arcsec(*driver, kTargetDec) < kConformUToleranceArcsec);
    driver->set_connected(false);
}

// Reads RightAscension at phase 0.9 of the fake's second, on a fresh handset read (past the 2 s position cache).
double ra_error_at_phase_09(alpacacore::TelescopeDriver& driver, const LandingOffsetHandset& st) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    wait_for_sawtooth_phase(st, 0.9);
    return ra_error_arcsec(driver, kTargetRa);
}

TEST_CASE("SynScan GOTO landing - a tracking write, a time write and a reconnect drop the sawtooth anchor (#1030)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ra_offset_counts.store(165);
    st->sawtooth_epoch.store(Clock::now().time_since_epoch().count());
    st->sawtooth.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);
    // Uncorrected, a phase-0.9 read is 0.9 s of RA (13.5") low; corrected it is within 3".
    constexpr double kRawPhase09Arcsec = 8.0;

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));
    CHECK(ra_error_at_phase_09(*driver, *st) < 3.0);
    driver->set_tracking(true);  // a T write, the same mode: the handset's regime may have changed
    CHECK(ra_error_at_phase_09(*driver, *st) > kRawPhase09Arcsec);

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));  // re-anchors
    CHECK(ra_error_at_phase_09(*driver, *st) < 3.0);
    driver->set_utc_date(std::chrono::system_clock::now());  // an H write resets the handset's clock
    CHECK(ra_error_at_phase_09(*driver, *st) > kRawPhase09Arcsec);

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));
    CHECK(ra_error_at_phase_09(*driver, *st) < 3.0);
    driver->set_connected(false);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(ra_error_at_phase_09(*driver, *st) > kRawPhase09Arcsec);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - AbortSlew during a refinement pass stops it (#880)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->ra_offset_counts.store(150);  // 11.6", within reach of an 8 s move at 2"/s, which takes 4 s
    driver->set_guide_rate(alpacacore::GuideRate{2.0 / 3600.0, 2.0 / 3600.0});

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + std::chrono::seconds(40);
    while (st->nudge_starts.load() < 1 && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(st->nudge_starts.load() == 1);  // the landing move is under way

    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK_FALSE(driver->get_slewing());
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK_FALSE(driver->get_slewing());
    CHECK(st->nudge_count.load() == 1);  // the move was stopped at once, not after its 4 s
    CHECK(st->goto_count.load() == 1);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a landing the handset re-rolls by one second of RA still converges (#1030)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ra_offset_counts.store(165);
    st->reroll.store(true);
    st->sawtooth_epoch.store(Clock::now().time_since_epoch().count());
    st->sawtooth.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    for (int slew = 0; slew < 2; ++slew) {  // the second GOTO is the re-rolled one
        REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));
        CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
        CHECK(dec_error_arcsec(*driver, kTargetDec) < 3.0);
    }
    CHECK(st->goto_count.load() == 2);  // one per slew
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a residual beyond a timed move's reach gets one re-GOTO first (#1030)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ra_offset_counts.store(1300);  // 100": beyond 8 s at the default 7.5"/s guide rate
    st->reroll.store(true);            // the re-GOTO lands a second east, so a move still finishes it
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));

    CHECK(st->goto_count.load() == 2);  // the GOTO and one re-GOTO, then timed moves
    CHECK(st->nudge_count.load() >= 1);
    CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
    driver->set_connected(false);
}

namespace {

// A GOTO that stays in progress for 6 s, started asynchronously. Every stop path below must return at once
// while the async slew task is still waiting on the handset (the Platform 7 Disconnect budget is 5 s).
constexpr auto kStopBudget = std::chrono::milliseconds(1500);

struct LongGoto {
    std::shared_ptr<LandingOffsetHandset> st = std::make_shared<LandingOffsetHandset>();
    std::unique_ptr<alpacacore::test::FakeMountServer> server;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    LongGoto() {
        st->goto_ms.store(6000);
        server = std::make_unique<alpacacore::test::FakeMountServer>(responder(st));
        REQUIRE(server->ok());
        driver = alpacacore::vendor::synscan::create_synscan_telescope(0, endpoint(server->port()),
                                                                       alpacacore::vendor::synscan::SynScanVersion::V4);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        REQUIRE(st->goto_count.load() == 1);
    }
};

}  // namespace

TEST_CASE("SynScan GOTO landing - Disconnect during an async GOTO returns at once (#880)",
          "[synscan][telescope][goto-landing]") {
    LongGoto g;
    const auto t0 = Clock::now();
    g.driver->set_connected(false);
    CHECK(Clock::now() - t0 < kStopBudget);
}

TEST_CASE("SynScan GOTO landing - MoveAxis during an async GOTO returns at once (#880)",
          "[synscan][telescope][goto-landing]") {
    LongGoto g;
    const auto t0 = Clock::now();
    try {
        g.driver->move_axis(0, 1.0);
    } catch (const alpacacore::AlpacaException&) {
        // the call may refuse the rate; only its latency is under test
    }
    CHECK(Clock::now() - t0 < kStopBudget);
    g.driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a second async slew replaces a waiting one at once (#880)",
          "[synscan][telescope][goto-landing]") {
    LongGoto g;
    const auto t0 = Clock::now();
    REQUIRE_NOTHROW(g.driver->slew_to_coordinates_async(kTargetRa + 1.0, kTargetDec));
    CHECK(Clock::now() - t0 < kStopBudget);
    g.driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - destroying the driver during an async GOTO returns at once (#880)",
          "[synscan][telescope][goto-landing]") {
    LongGoto g;
    const auto t0 = Clock::now();
    g.driver.reset();
    CHECK(Clock::now() - t0 < kStopBudget);
}

#endif  // !_WIN32
