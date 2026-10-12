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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/zwo/zwo_camera_driver.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "fake_zwo_sdk.h"

namespace {

using alpacacore::AlpacaException;
using alpacacore::test::FakeZWOSDK;
using alpacacore::vendor::zwo::create_zwo_camera;
namespace AlpacaError = alpacacore::AlpacaError;

void connect_camera(alpacacore::CameraDriver& camera) {
    camera.set_connected(true);
    REQUIRE(camera.get_connected());
}

std::vector<std::uint8_t> make_raw16_frame(int width, int height) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 2);
    for (int i = 0; i < width * height; ++i) {
        const auto value = static_cast<std::uint16_t>(i + 1);
        bytes[static_cast<std::size_t>(i) * 2] = static_cast<std::uint8_t>(value & 0xff);
        bytes[static_cast<std::size_t>(i) * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
    }
    return bytes;
}

int error_code(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const AlpacaException& ex) {
        return ex.error_code();
    }
    return -1;
}

}  // namespace

TEST_CASE("ZWO fake SDK - lazy download preserves padded ROI pixels", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);

    camera->set_num_x(7);
    camera->set_num_y(3);
    camera->set_start_x(9);  // the padded eight-pixel ROI shifts left and crops one column
    camera->set_start_y(5);  // the effective ROI shifts upward and crops one source row
    sdk.frame_bytes = make_raw16_frame(8, 4);
    CHECK(error_code([&] { camera->start_exposure(std::numeric_limits<double>::quiet_NaN(), true); }) ==
          AlpacaError::InvalidValue);
    camera->start_exposure(0.001, true);

    CHECK(camera->get_image_ready());
    CHECK(sdk.call_count("get_data_after_exposure") == 0);  // readiness never starts the USB bulk transfer
    CHECK(error_code([&] { camera->set_num_x(8); }) == AlpacaError::InvalidOperation);

    const auto image = camera->get_image_array();
    REQUIRE(image.rank == 2);
    REQUIRE(image.width == 7);
    REQUIRE(image.height == 3);
    CHECK(image.data[0] == 10);
    CHECK(image.data[6] == 16);
    CHECK(image.data[14] == 26);
    CHECK(camera->get_image_ready());
    CHECK(sdk.call_count("get_data_after_exposure") == 1);
    camera->set_num_x(8);
    CHECK_FALSE(camera->get_image_ready());
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::InvalidOperation);
}

TEST_CASE("ZWO fake SDK - full-frame binned dimensions survive ROI alignment", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.camera.max_width = 6248;
    sdk.camera.max_height = 4178;
    sdk.camera.supported_bins = {1, 2, 3, 4};
    sdk.frame_bytes.assign(static_cast<std::size_t>(6248 / 2) * (4178 / 2) * 2, 0xff);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);

    for (const int bin : {2, 3, 4}) {
        camera->set_bin_x(bin);
        camera->start_exposure(0.001, true);
        const auto image = camera->get_image_array();
        const int requested_width = 6248 / bin;
        const int requested_height = 4178 / bin;
        REQUIRE(image.rank == 2);
        CHECK(image.width == requested_width);
        CHECK(image.height == requested_height);
        CHECK(image.data.size() == static_cast<std::size_t>(requested_width) * requested_height);
        CHECK(image.data.front() == 65535);
        CHECK(image.data[static_cast<std::size_t>(requested_width - 1)] == 0);
        if (bin == 2) {
            const auto penultimate_row = static_cast<std::size_t>(requested_height - 2) * requested_width;
            const auto last_row = static_cast<std::size_t>(requested_height - 1) * requested_width;
            CHECK(image.data[penultimate_row] == 65535);
            CHECK(image.data[last_row] == 0);
        }
    }
}

TEST_CASE("ZWO fake SDK - failed exposure replacement preserves the previous image", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    const auto previous = camera->get_image_array();
    REQUIRE(camera->get_image_ready());

    sdk.fail_exposure_register_write = true;
    CHECK(error_code([&] { camera->start_exposure(0.002, true); }) == AlpacaError::DriverException);
    CHECK(camera->get_last_exposure_duration() == 0.001);
    CHECK(camera->get_image_ready());
    CHECK(camera->get_image_array().data == previous.data);

    sdk.fail_exposure_start = true;
    CHECK(error_code([&] { camera->start_exposure(0.003, true); }) == AlpacaError::DriverException);
    CHECK(camera->get_last_exposure_duration() == 0.001);
    CHECK(camera->get_image_ready());
    CHECK(camera->get_image_array().data == previous.data);
    CHECK(sdk.call_count("get_data_after_exposure") == 1);
}

TEST_CASE("ZWO fake SDK - post-open connect failure closes the handle and can reconnect", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.camera.supported_formats.clear();
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);

    CHECK(error_code([&] { camera->set_connected(true); }) == AlpacaError::DriverException);
    CHECK_FALSE(camera->get_connected());
    CHECK(sdk.open_count() == 0);
    CHECK(sdk.call_count("close_camera") == 1);

    sdk.camera.supported_formats = {FakeZWOSDK::ImageType::Raw16};
    camera->set_connected(true);
    CHECK(camera->get_connected());
    CHECK(sdk.open_count() == 1);
    camera->set_connected(false);
    CHECK(sdk.open_count() == 0);
}

TEST_CASE("ZWO fake SDK - geometry changes preserve last-exposure metadata", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    REQUIRE_NOTHROW(camera->get_image_array());
    const auto last_start = camera->get_last_exposure_start_time();
    const double last_duration = camera->get_last_exposure_duration();
    const auto check_history = [&] {
        CHECK(camera->get_last_exposure_start_time() == last_start);
        CHECK(camera->get_last_exposure_duration() == last_duration);
    };

    camera->set_bin_x(2);
    check_history();
    camera->set_bin_y(2);
    check_history();
    camera->set_num_x(7);
    check_history();
    camera->set_num_y(3);
    check_history();
    camera->set_start_x(1);
    check_history();
    camera->set_start_x(1);
    check_history();
    camera->set_start_y(1);
    check_history();
    CHECK_FALSE(camera->get_image_ready());
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::InvalidOperation);
}

TEST_CASE("ZWO fake SDK - overlapping start is rejected without replacing the active exposure", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.auto_complete_exposure = false;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);

    CHECK_FALSE(camera->get_image_ready());
    const int starts = sdk.call_count("start_exposure");
    CHECK(error_code([&] { camera->start_exposure(0.002, true); }) == AlpacaError::InvalidOperation);
    CHECK(sdk.call_count("start_exposure") == starts);
    CHECK_FALSE(camera->get_image_ready());

    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Success);
    CHECK(camera->get_image_ready());
}

TEST_CASE("ZWO fake SDK - RGB24 download converts BGR bytes to RGB channels", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.camera.is_color = true;
    sdk.camera.supported_formats = {FakeZWOSDK::ImageType::Rgb24};
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->set_num_x(7);
    camera->set_num_y(3);
    camera->set_start_x(9);
    sdk.frame_bytes.resize(8 * 4 * 3);
    for (std::size_t pixel = 0; pixel < 8 * 4; ++pixel) {
        const auto offset = pixel * 3;
        sdk.frame_bytes[offset] = static_cast<std::uint8_t>(pixel * 3 + 3);      // B
        sdk.frame_bytes[offset + 1] = static_cast<std::uint8_t>(pixel * 3 + 2);  // G
        sdk.frame_bytes[offset + 2] = static_cast<std::uint8_t>(pixel * 3 + 1);  // R
    }
    camera->start_exposure(0.001, true);

    const auto image = camera->get_image_array();
    REQUIRE(image.rank == 3);
    REQUIRE(image.width == 7);
    REQUIRE(image.height == 3);
    CHECK(image.data[0] == 4);
    CHECK(image.data[1] == 5);
    CHECK(image.data[2] == 6);
    CHECK(image.data[3] == 7);
    CHECK(image.data[4] == 8);
    CHECK(image.data[5] == 9);
}

TEST_CASE("ZWO fake SDK - lazy download failure stays visible and later exposure recovers", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    sdk.download_error = "scripted bulk-transfer failure";
    camera->start_exposure(0.001, true);

    REQUIRE(camera->get_image_ready());
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
    CHECK(sdk.call_count("get_data_after_exposure") == 1);

    camera->abort_exposure();
    CHECK_FALSE(camera->get_image_ready());
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::InvalidOperation);

    sdk.download_error.clear();
    sdk.frame_bytes = make_raw16_frame(16, 8);
    camera->start_exposure(0.001, true);
    REQUIRE(camera->get_image_ready());
    const auto image = camera->get_image_array();
    CHECK(image.width == 16);
    CHECK(image.height == 8);
    CHECK(image.data.front() == 1);
    CHECK(camera->get_image_ready());
    CHECK(sdk.call_count("get_data_after_exposure") == 2);
}

TEST_CASE("ZWO fake SDK - status read failure and exhausted retries stay visible and recover", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);

    sdk.set_exposure_status_error("scripted status-read failure");
    camera->start_exposure(0.001, true);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);

    sdk.set_exposure_status_error("");
    camera->start_exposure(0.001, true);
    sdk.script_exposure_status(
        {FakeZWOSDK::ExposureStatus::Failed, FakeZWOSDK::ExposureStatus::Failed, FakeZWOSDK::ExposureStatus::Failed});
    CHECK_FALSE(camera->get_image_ready());
    std::this_thread::sleep_for(std::chrono::milliseconds(260));
    CHECK_FALSE(camera->get_image_ready());
    std::this_thread::sleep_for(std::chrono::milliseconds(260));
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);

    camera->start_exposure(0.001, true);
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_image_array().data.front() == 1);
}

TEST_CASE("ZWO fake SDK - reconnect clears status, ROI and transfer failures", "[zwo][camera][unit]") {
    enum class Failure { Status, Roi, Download };
    for (const Failure failure : {Failure::Status, Failure::Roi, Failure::Download}) {
        FakeZWOSDK sdk;
        sdk.frame_bytes = make_raw16_frame(16, 8);
        auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
        connect_camera(*camera);
        camera->start_exposure(0.001, true);

        if (failure == Failure::Status) {
            sdk.set_exposure_status_error("scripted status-read failure");
            CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
        } else if (failure == Failure::Roi) {
            sdk.roi_readback_override = FakeZWOSDK::ROIDescription{0, 8, 1, FakeZWOSDK::ImageType::Raw16};
            CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
        } else {
            sdk.download_error = "scripted bulk-transfer failure";
            REQUIRE(camera->get_image_ready());
            CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
        }

        camera->set_connected(false);
        sdk.set_exposure_status_error("");
        sdk.roi_readback_override.reset();
        sdk.download_error.clear();
        camera->set_connected(true);
        CHECK_FALSE(camera->get_image_ready());
        CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::InvalidOperation);

        camera->start_exposure(0.001, true);
        REQUIRE(camera->get_image_ready());
        CHECK(camera->get_image_array().data.front() == 1);
        camera->set_connected(false);
        CHECK(sdk.open_count() == 0);
    }
}

TEST_CASE("ZWO fake SDK - successful raw16 conversion preserves values above one byte", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.camera.max_width = 32;
    sdk.camera.max_height = 16;
    sdk.frame_bytes = make_raw16_frame(32, 16);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);

    const auto image = camera->get_image_array();
    REQUIRE(image.rank == 2);
    CHECK(image.data[255] == 256);
}

TEST_CASE("ZWO fake SDK - Raw8 and Y8 formats publish rank-two images", "[zwo][camera][unit]") {
    for (const auto format : {FakeZWOSDK::ImageType::Raw8, FakeZWOSDK::ImageType::Y8}) {
        FakeZWOSDK sdk;
        sdk.camera.supported_formats = {format};
        sdk.frame_bytes.resize(16 * 8, 0x5a);
        auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
        connect_camera(*camera);
        camera->start_exposure(0.001, true);

        const auto image = camera->get_image_array();
        CHECK(image.rank == 2);
        CHECK(image.data.front() == 0x5a);
    }
}

TEST_CASE("ZWO fake SDK - ImageReady cannot pass an in-flight poll after download failure", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.download_error = "scripted bulk-transfer failure";
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);

    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool poll_entered = false;
    bool release_poll = false;
    std::atomic<int> status_calls{0};
    sdk.before_call = [&](const std::string& name) {
        if (name != "get_exposure_status" || status_calls.fetch_add(1) != 0) return;
        std::unique_lock lock(gate_mutex);
        poll_entered = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release_poll; });
    };

    std::atomic<int> ready_error{0};
    std::thread ready_reader([&] {
        try {
            static_cast<void>(camera->get_image_ready());
        } catch (const AlpacaException& ex) {
            ready_error.store(ex.error_code());
        }
    });
    bool first_poll_held = false;
    {
        std::unique_lock lock(gate_mutex);
        first_poll_held = gate_cv.wait_for(lock, std::chrono::seconds(2), [&] { return poll_entered; });
    }
    if (!first_poll_held) {
        {
            std::lock_guard lock(gate_mutex);
            release_poll = true;
        }
        gate_cv.notify_all();
        ready_reader.join();
        CHECK(first_poll_held);
        return;
    }

    const int array_error = error_code([&] { static_cast<void>(camera->get_image_array()); });
    const bool download_seen = sdk.wait_for_call_count("get_data_after_exposure", 1, std::chrono::seconds(2));
    {
        std::lock_guard lock(gate_mutex);
        release_poll = true;
    }
    gate_cv.notify_all();
    ready_reader.join();

    CHECK(download_seen);
    CHECK(array_error == AlpacaError::DriverException);
    CHECK(ready_error.load() == AlpacaError::DriverException);
}

TEST_CASE("ZWO fake SDK - concurrent ImageArray reads share one lazy transfer", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    REQUIRE(camera->get_image_ready());
    sdk.hold_download();

    alpacacore::ImageArray first;
    alpacacore::ImageArray second;
    std::atomic<int> first_code{0};
    std::atomic<int> second_code{0};
    std::atomic<bool> second_started{false};
    std::thread first_reader([&] {
        try {
            first = camera->get_image_array();
        } catch (const AlpacaException& ex) {
            first_code.store(ex.error_code());
        }
    });
    const bool transfer_started = sdk.wait_for_download(std::chrono::seconds(2));
    std::thread second_reader([&] {
        second_started.store(true);
        try {
            second = camera->get_image_array();
        } catch (const AlpacaException& ex) {
            second_code.store(ex.error_code());
        }
    });
    while (!second_started.load()) std::this_thread::yield();
    const bool second_reached_sdk = sdk.wait_for_call_count("get_roi_format", 3, std::chrono::milliseconds(100));
    CHECK_FALSE(second_reached_sdk);
    CHECK(sdk.call_count("get_data_after_exposure") == 1);
    sdk.release_download();
    first_reader.join();
    second_reader.join();

    CHECK(transfer_started);
    CHECK(first_code.load() == 0);
    CHECK(second_code.load() == 0);
    CHECK(sdk.call_count("get_data_after_exposure") == 1);
    CHECK(first.data == second.data);
}

TEST_CASE("ZWO fake SDK - mismatched ROI metadata cannot publish ready", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    for (const auto& roi : {FakeZWOSDK::ROIDescription{0, 8, 1, FakeZWOSDK::ImageType::Raw16},
                            FakeZWOSDK::ROIDescription{8, 8, 1, FakeZWOSDK::ImageType::Raw16},
                            FakeZWOSDK::ROIDescription{16, 6, 1, FakeZWOSDK::ImageType::Raw16},
                            FakeZWOSDK::ROIDescription{16, 8, 2, FakeZWOSDK::ImageType::Raw16},
                            FakeZWOSDK::ROIDescription{16, 8, 1, FakeZWOSDK::ImageType::Raw8}}) {
        sdk.roi_readback_override = roi;
        camera->start_exposure(0.001, true);
        CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
        CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
        CHECK(sdk.call_count("get_data_after_exposure") == 0);
    }

    sdk.roi_readback_override.reset();
    sdk.start_readback_override = FakeZWOSDK::StartPosition{1, 0};
    camera->start_exposure(0.001, true);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
    CHECK(sdk.call_count("get_data_after_exposure") == 0);

    sdk.start_readback_override.reset();
    sdk.frame_bytes = make_raw16_frame(16, 8);
    camera->start_exposure(0.001, true);
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_image_array().data.front() == 1);
}

TEST_CASE("ZWO fake SDK - abort invalidates an in-flight lazy download", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    sdk.hold_download();

    std::atomic<int> result{-1};
    std::thread download([&] { result.store(error_code([&] { static_cast<void>(camera->get_image_array()); })); });
    const bool download_started = sdk.wait_for_download(std::chrono::seconds(2));
    camera->abort_exposure();
    const int stop_calls = sdk.call_count("stop_exposure");
    sdk.release_download();
    download.join();

    CHECK(download_started);
    CHECK(stop_calls > 0);
    CHECK(result.load() == AlpacaError::InvalidOperation);
    CHECK_FALSE(camera->get_image_ready());
    CHECK(sdk.call_count("get_data_after_exposure") == 1);

    camera->start_exposure(0.001, true);
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_image_array().data.front() == 1);
}

TEST_CASE("ZWO fake SDK - disconnect waits for the lazy download before close", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    sdk.stop_releases_download = false;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    sdk.hold_download();

    std::atomic<int> download_code{0};
    std::atomic<bool> disconnected{false};
    std::thread download(
        [&] { download_code.store(error_code([&] { static_cast<void>(camera->get_image_array()); })); });
    const bool download_started = sdk.wait_for_download(std::chrono::seconds(2));
    std::thread disconnect([&] {
        camera->set_connected(false);
        disconnected.store(true);
    });
    const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (sdk.call_count("stop_exposure") == 0 && std::chrono::steady_clock::now() < stop_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool stop_seen = sdk.call_count("stop_exposure") > 0;
    const bool close_waited = !disconnected.load() && sdk.open_count() == 1 && sdk.call_count("close_camera") == 0;
    sdk.release_download();
    download.join();
    disconnect.join();

    CHECK(download_started);
    CHECK(stop_seen);
    CHECK(close_waited);
    CHECK(download_code.load() == AlpacaError::InvalidOperation);
    CHECK_FALSE(sdk.closed_during_download());
    CHECK(sdk.open_count() == 0);
}
