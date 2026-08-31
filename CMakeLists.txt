# tincan — SIP voice endpoint for the LilyGO T3-S3 MVSRBoard
# ESP-IDF top-level project file.
cmake_minimum_required(VERSION 3.16)

# Shared endpoint core (sip_core parser, TincanUac, PlayoutBuffer/rtp, G.711)
# lives in the sibling tincan-core repo. Default expects a sibling checkout;
# CI or a nonstandard layout sets TINCAN_CORE_PATH.
if(DEFINED ENV{TINCAN_CORE_PATH})
    set(TINCAN_CORE_PATH $ENV{TINCAN_CORE_PATH})
else()
    get_filename_component(TINCAN_CORE_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/../tincan-core" ABSOLUTE)
endif()
if(NOT EXISTS "${TINCAN_CORE_PATH}/components/tincan_uac/CMakeLists.txt")
    message(FATAL_ERROR "tincan-core not found at ${TINCAN_CORE_PATH} -- "
        "clone github.com/GlomarGadaffi/tincan-core alongside this repo or "
        "set TINCAN_CORE_PATH")
endif()
set(EXTRA_COMPONENT_DIRS "${TINCAN_CORE_PATH}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tincan)
