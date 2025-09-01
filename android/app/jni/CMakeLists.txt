cmake_minimum_required(VERSION 3.22)
project(ManiaAndroid LANGUAGES C CXX)

# Stronger release opts for AArch64 (safe for this codebase)
set(CMAKE_C_FLAGS_RELEASE   "${CMAKE_C_FLAGS_RELEASE} -O3 -funroll-loops -fstrict-aliasing")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -funroll-loops -fstrict-aliasing")

# Keep Debug friendly; just make Release fast.

# Make cross-dir target_link_libraries legal in modern CMake graphs
if(POLICY CMP0079)
  cmake_policy(SET CMP0079 NEW)
endif()

if (NOT ANDROID)
  message(FATAL_ERROR "This CMakeLists is intended to be used from Android/Gradle.")
endif()

# ── Paths ──────────────────────────────────────────────────────────────────────
# This file lives at:
#   <repo>/dependencies/RSDKv5/android/app/jni/CMakeLists.txt
# Game root is five levels up:
#   jni → app → android → RSDKv5 → dependencies → <Sonic-Mania-Decompilation>
set(GAME_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../../../)

# (Optional) expose RSDK root to the game build if it needs an explicit hint
set(RSDK_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../.. CACHE PATH "" FORCE)

# ── Configure game to produce libGame.so and use RSDK ──────────────────────────
# These cache vars match what you pass from Gradle; kept here as a safety net.
set(WITH_RSDK ON CACHE BOOL "" FORCE)
set(GAME_STATIC OFF CACHE BOOL "" FORCE)
set(GAME_NAME Game CACHE STRING "" FORCE)

# ── Add ONLY the game; it will add RSDKv5 internally ───────────────────────────
add_subdirectory(${GAME_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/game-out)

# Helpful in multi-variant Studio sessions
set_property(GLOBAL PROPERTY ALLOW_DUPLICATE_CUSTOM_TARGETS TRUE)
