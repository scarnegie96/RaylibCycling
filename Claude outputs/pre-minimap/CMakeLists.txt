cmake_minimum_required(VERSION 3.21)
project(CoreRay VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(CORERAY_WITH_BLE "Build with SimpleBLE (real trainer / HRM support)" ON)
option(CORERAY_TESTS "Build protocol parser tests" ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------- raylib
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
set(CUSTOMIZE_BUILD OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(raylib)

# ---------------------------------------------------------------- SimpleBLE
if(CORERAY_WITH_BLE)
    set(SIMPLEBLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(simpleble
        GIT_REPOSITORY https://github.com/simpleble/simpleble.git
        GIT_TAG v1.4.0
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR simpleble)
    FetchContent_MakeAvailable(simpleble)
endif()

# ---------------------------------------------------------------- app
# Pure core (no raylib / BLE): engine, physics, workouts, tests, simulator, export
add_library(coreray_core STATIC
    src/Protocol.cpp
    src/core/Physics.cpp
    src/core/Metrics.cpp
    src/core/Workout.cpp
    src/core/Sim.cpp
    src/core/Engine.cpp
    src/core/Export.cpp
    src/core/Dates.cpp
    src/core/Route.cpp
    src/core/Records.cpp
    src/core/Weather.cpp
)
target_include_directories(coreray_core PUBLIC src)
target_compile_features(coreray_core PUBLIC cxx_std_17)

# SQLite (ride history, routes, events - CoreKick's schema). Ships with macOS.
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

# Storage + networking (SQLite, curl via the shell, OAuth loopback)
add_library(coreray_store STATIC src/Db.cpp src/Net.cpp)
# CMake 4.1+ names the target SQLite3::SQLite3 (SQLite::SQLite3 is deprecated); older ones only have the latter
if(TARGET SQLite3::SQLite3)
    set(CORERAY_SQLITE_TARGET SQLite3::SQLite3)
else()
    set(CORERAY_SQLITE_TARGET SQLite::SQLite3)
endif()
target_link_libraries(coreray_store PUBLIC coreray_core ${CORERAY_SQLITE_TARGET} Threads::Threads)

# Rigged human body (pure C++)
add_library(coreray_body STATIC src/body/Body.cpp)
target_include_directories(coreray_body PUBLIC src)
target_compile_features(coreray_body PUBLIC cxx_std_17)
target_link_libraries(coreray_body PUBLIC Threads::Threads)

# Whole-world generator: climate, biomes, hydrology, towns, road network (pure C++)
add_library(coreray_wmap STATIC src/world/WorldMap.cpp src/world/WorldRoads.cpp src/world/WorldSample.cpp)
target_include_directories(coreray_wmap PUBLIC src)
target_compile_features(coreray_wmap PUBLIC cxx_std_17)
target_link_libraries(coreray_wmap PUBLIC Threads::Threads)

# Course layout + terrain generation (pure C++, threads only)
add_library(coreray_world STATIC src/Course.cpp src/Terrain.cpp)
target_link_libraries(coreray_world PUBLIC coreray_core coreray_wmap Threads::Threads)

add_executable(CoreRay
    src/main.cpp
    src/App.cpp
    src/HomeUi.cpp
    src/LibraryUi.cpp
    src/EventsUi.cpp
    src/HistoryUi.cpp
    src/SettingsUi.cpp
    src/ExploreUi.cpp
    src/RoamUi.cpp
    src/FreeWorldUi.cpp
    src/MapPreview.cpp
    src/RideUi.cpp
    src/SummaryUi.cpp
    src/Audio.cpp
    src/Ble.cpp
    src/Renderer.cpp
    src/World.cpp
    src/Structures.cpp
    src/Fx.cpp
    src/Volumetrics.cpp
    src/Profiler.cpp
    src/Impostors.cpp
    src/Animals.cpp
    src/Textures.cpp
    src/Trees.cpp
    src/EzTree.cpp
    src/Rider.cpp
    src/Store.cpp
    src/Ui.cpp
)
target_link_libraries(CoreRay PRIVATE coreray_core coreray_world coreray_store coreray_body raylib)
if(CORERAY_WITH_BLE)
    target_link_libraries(CoreRay PRIVATE simpleble::simpleble)
    target_compile_definitions(CoreRay PRIVATE CORERAY_BLE=1)
endif()

target_compile_definitions(CoreRay PRIVATE CORERAY_SQLITE=1)

if(APPLE)
    target_link_libraries(CoreRay PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL"
                                          "-framework CoreBluetooth" "-framework Foundation")
    # Embed an Info.plist in the plain executable so macOS shows the Bluetooth
    # permission prompt with a proper description (works when run from Terminal).
    target_link_options(CoreRay PRIVATE
        "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist")
    # the Dock icon (macos/DockIcon.cpp, plain C++ via the Objective-C runtime, from
    # assets/icon/coreray-1024.png); macos/CoreRay.icns is the same icon for an .app bundle
    target_sources(CoreRay PRIVATE macos/DockIcon.cpp)
    target_link_libraries(CoreRay PRIVATE objc)
    target_compile_definitions(CoreRay PRIVATE CORERAY_DOCK_ICON=1)
endif()

if(MSVC)
    target_compile_options(CoreRay PRIVATE /W3)
else()
    target_compile_options(CoreRay PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter)
endif()

# World map: generate a world and write maps of it
add_executable(world_map tools/world_map.cpp)
target_link_libraries(world_map PRIVATE coreray_wmap raylib)
if(APPLE)
    target_link_libraries(world_map PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
endif()

# Body viewer: inspect the rigged greybox human (modes, skeleton, weights)
add_executable(body_viewer tools/body_viewer.cpp)
target_link_libraries(body_viewer PRIVATE coreray_body raylib)
if(APPLE)
    target_link_libraries(body_viewer PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
endif()

# Copy assets next to the binary
add_custom_command(TARGET CoreRay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:CoreRay>/assets)

if(CORERAY_TESTS)
    enable_testing()
    add_executable(protocol_tests tests/protocol_tests.cpp)
    target_link_libraries(protocol_tests PRIVATE coreray_core)
    add_test(NAME protocol_tests COMMAND protocol_tests)
    add_executable(core_tests tests/core_tests.cpp)
    target_link_libraries(core_tests PRIVATE coreray_core)
    add_test(NAME core_tests COMMAND core_tests)
    add_executable(history_tests tests/history_tests.cpp)
    target_link_libraries(history_tests PRIVATE coreray_core)
    add_test(NAME history_tests COMMAND history_tests)
    add_executable(db_tests tests/db_tests.cpp)
    target_link_libraries(db_tests PRIVATE coreray_store)
    add_test(NAME db_tests COMMAND db_tests)
    add_executable(terrain_tests tests/terrain_tests.cpp)
    target_link_libraries(terrain_tests PRIVATE coreray_world)
    add_test(NAME terrain_tests COMMAND terrain_tests)
    add_executable(world_tests tests/world_tests.cpp)
    target_link_libraries(world_tests PRIVATE coreray_wmap)
    add_test(NAME world_tests COMMAND world_tests)
    add_executable(body_tests tests/body_tests.cpp)
    target_link_libraries(body_tests PRIVATE coreray_body)
    add_test(NAME body_tests COMMAND body_tests)
    # development: relief maps of the generated terrain
    add_executable(terrain_dump tools/terrain_dump.cpp)
    target_link_libraries(terrain_dump PRIVATE coreray_world)
endif()
