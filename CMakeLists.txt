cmake_minimum_required(VERSION 3.20)
project(storage_tiering VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

# ── Dependencies via FetchContent ──────────────────────────
include(FetchContent)

# SQLite3 amalgamation
FetchContent_Declare(
  sqlite3
  URL https://www.sqlite.org/2025/sqlite-amalgamation-3480000.zip
)
FetchContent_MakeAvailable(sqlite3)

# SQLite3 amalgamation has no CMakeLists.txt, so build it explicitly
add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})

# nlohmann/json (header-only)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

# cpp-httplib (header-only)
FetchContent_Declare(
  httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.0.tar.gz
)
FetchContent_MakeAvailable(httplib)

# yaml-cpp
FetchContent_Declare(
  yaml-cpp
  URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
)
FetchContent_MakeAvailable(yaml-cpp)

# Google Test
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
)
FetchContent_MakeAvailable(googletest)

# ── Sources ────────────────────────────────────────────────
set(TIERING_SOURCES
    src/tiering/heat_map.cpp
    src/tiering/iops_monitor.cpp
    src/tiering/io_profiler.cpp
    src/tiering/hysteresis.cpp
    src/tiering/wear_leveler.cpp
    src/tiering/nvram_cache.cpp
    src/tiering/relocation.cpp
)

set(CORE_SOURCES
    ${TIERING_SOURCES}
    src/catalog/persistent_catalog.cpp
    src/policy/recency_scorer.cpp
    src/policy/frequency_scorer.cpp
    src/policy/type_scorer.cpp
    src/policy/size_scorer.cpp
    src/policy/composite_scorer.cpp
    src/policy/lifecycle_rule.cpp
    src/policy/policy_engine.cpp
    src/migration/local_migrator.cpp
    src/migration/smb_migrator.cpp
    src/migration/migration_planner.cpp
    src/migration/migration_engine.cpp
    src/scanner/real_scanner.cpp
    src/api/rest_server.cpp
    src/api/s3_server.cpp
    src/api/auth.cpp
    src/api/fileserver.cpp
    src/api/supabase_client.cpp
    src/monitoring/file_watcher.cpp
    src/monitoring/access_tracker.cpp
    src/monitoring/drive_detector.cpp
    src/dashboard.cpp
    src/logger.cpp
    src/config.cpp
)

set(ALL_SOURCES
    src/main.cpp
    ${CORE_SOURCES}
)

set(TIERING_HEADERS
    include/tiering/extent.hpp
    include/tiering/heat_map.hpp
    include/tiering/iops_monitor.hpp
    include/tiering/io_profiler.hpp
    include/tiering/hysteresis.hpp
    include/tiering/wear_leveler.hpp
    include/tiering/nvram_cache.hpp
    include/tiering/relocation.hpp
)

set(ALL_HEADERS
    ${TIERING_HEADERS}
    include/core/tier.hpp
    include/core/file_record.hpp
    include/core/types.hpp
    include/catalog/catalog_interface.hpp
    include/catalog/persistent_catalog.hpp
    include/policy/scorer.hpp
    include/policy/recency_scorer.hpp
    include/policy/frequency_scorer.hpp
    include/policy/type_scorer.hpp
    include/policy/size_scorer.hpp
    include/policy/composite_scorer.hpp
    include/policy/lifecycle_rule.hpp
    include/policy/policy_engine.hpp
    include/migration/migrator_interface.hpp
    include/migration/local_migrator.hpp
    include/migration/smb_migrator.hpp
    include/migration/migration_planner.hpp
    include/migration/migration_engine.hpp
    include/scanner/real_scanner.hpp
    include/api/rest_server.hpp
    include/api/s3_server.hpp
    include/api/s3_types.hpp
    include/api/auth.hpp
    include/api/fileserver.hpp
    include/dashboard.hpp
    include/logger.hpp
    include/config.hpp
    include/monitoring/file_watcher.hpp
    include/monitoring/access_tracker.hpp
    include/monitoring/drive_detector.hpp
)

# ── Library ────────────────────────────────────────────────
add_library(tiering_core STATIC ${CORE_SOURCES} ${ALL_HEADERS})
target_include_directories(tiering_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${sqlite3_SOURCE_DIR}
    ${nlohmann_json_SOURCE_DIR}/include
    ${httplib_SOURCE_DIR}
    ${yaml-cpp_SOURCE_DIR}/include
)
target_link_libraries(tiering_core PUBLIC
    sqlite3
    nlohmann_json::nlohmann_json
    httplib::httplib
    yaml-cpp
    Threads::Threads
)
if(WIN32)
    target_link_libraries(tiering_core PUBLIC mpr bcrypt winhttp)
endif()

# ── Executable ─────────────────────────────────────────────
add_executable(storage_tiering ${ALL_SOURCES} ${ALL_HEADERS})
target_include_directories(storage_tiering PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${sqlite3_SOURCE_DIR}
    ${nlohmann_json_SOURCE_DIR}/include
    ${httplib_SOURCE_DIR}
    ${yaml-cpp_SOURCE_DIR}/include
)
target_link_libraries(storage_tiering PRIVATE tiering_core)

# Copy dashboard HTML next to the executable
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/api/dashboard.html
    ${CMAKE_CURRENT_BINARY_DIR}/Release/dashboard.html
    COPYONLY
)

# Copy file server HTML next to the executable
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/api/fileserver.html
    ${CMAKE_CURRENT_BINARY_DIR}/Release/fileserver.html
    COPYONLY
)

# ── Tests ──────────────────────────────────────────────────
enable_testing()

add_executable(tiering_tests
    tests/test_main.cpp
    tests/test_catalog.cpp
    tests/test_policy.cpp
    tests/test_migration.cpp
)
target_include_directories(tiering_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${sqlite3_SOURCE_DIR}
    ${nlohmann_json_SOURCE_DIR}/include
    ${httplib_SOURCE_DIR}
    ${yaml-cpp_SOURCE_DIR}/include
)
target_link_libraries(tiering_tests PRIVATE
    tiering_core
    GTest::gtest
    GTest::gtest_main
)
add_test(NAME tiering_tests COMMAND tiering_tests)

# ── Install ────────────────────────────────────────────────
install(TARGETS storage_tiering RUNTIME DESTINATION bin)
install(FILES ${ALL_HEADERS} DESTINATION include/storage_tiering)
install(DIRECTORY config/ DESTINATION etc/storage_tiering)
