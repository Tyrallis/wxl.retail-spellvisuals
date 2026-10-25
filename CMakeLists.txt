cmake_minimum_required(VERSION 3.21)
project(wxl-retail-spellvisuals VERSION 0.2.0 LANGUAGES NONE)

set(WXL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the WarcraftXL SDK")
include("${WXL_SDK_DIR}/cmake/wxl_add_mod.cmake")

# x64 only: wxl.m2's world calls are published by the runtime the recompiled client loads.
wxl_add_mod(retail-spellvisuals
    SOURCES src/RetailSpellVisuals.cpp src/Glow.cpp
    MANIFEST mod.json
    OUTPUT_NAME wxl-retail-spellvisuals
    TARGETS win-x64)
