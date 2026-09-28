cmake_minimum_required(VERSION 3.20)

set(ASTROASIS_SOURCES
    astroasis_protocol_wrapper.cpp
    astroasis_focuser_driver.cpp
    astroasis_catalog.cpp
)

add_library(alpacacore_astroasis STATIC ${ASTROASIS_SOURCES})

target_include_directories(alpacacore_astroasis PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# Astroasis Oasis Focuser speaks a USB HID vendor protocol (VID:PID
# 338F:A0F0), reverse-engineered from the vendor's ASCOM driver -- no vendor
# SDK binary is used. Talks to the device via hidapi's hidraw backend
# (system package: libhidapi-dev / hidapi-hidraw pkg-config module).
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIDAPI REQUIRED IMPORTED_TARGET hidapi-hidraw)
target_link_libraries(alpacacore_astroasis PRIVATE PkgConfig::HIDAPI)

find_package(Threads REQUIRED)
target_link_libraries(alpacacore_astroasis PRIVATE Threads::Threads)

# open-astro#664: astroasis_focuser_driver.cpp calls into alpacacore's own
# logging/AsyncConnectable symbols, while alpacacore's device_catalog.cpp now
# calls register_astroasis_factory() (defined here) -- a genuine link cycle
# between the two static libraries. Declaring the edge in both directions
# lets CMake's generator repeat the libraries on the link line as needed; a
# small test binary that pulls in astroasis's catalog factory without also
# needing another arm's logging calls (e.g. test_catalog_json) undefined-
# references otherwise, because ld.bfd never revisits an archive it has
# already passed once.
target_link_libraries(alpacacore_astroasis PRIVATE alpacacore)
