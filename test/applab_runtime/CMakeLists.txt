# The vendored Lua 5.4.9 runtime, built with the same source filter and flags as
# lib/CrossStoreLua/library.json, plus App Lab's sandbox check.
set(APPLAB_LUA_DIR ${REPO_ROOT}/lib/CrossStoreLua/src)
file(GLOB APPLAB_LUA_SOURCES ${APPLAB_LUA_DIR}/*.c)
list(FILTER APPLAB_LUA_SOURCES EXCLUDE REGEX "/(lua|luac|linit|liolib|loslib|loadlib|ldblib|lcorolib)\\.c$")

add_library(applab_lua STATIC ${APPLAB_LUA_SOURCES})
target_include_directories(applab_lua PUBLIC ${APPLAB_LUA_DIR})
target_compile_definitions(applab_lua PUBLIC LUAI_MAXCCALLS=50)

add_executable(applab_runtime_tests
  AppLabRuntimeTest.cpp
  ${REPO_ROOT}/lib/CrossStoreRuntime/CrossStoreRuntime.cpp
  ${REPO_ROOT}/src/activities/apps/applab/AppLabCheck.cpp
  ${REPO_ROOT}/src/activities/apps/applab/AppLabProtocol.cpp
)
# stubs/ first: its esp_heap_caps.h stands in for ESP-IDF's.
target_include_directories(applab_runtime_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/CrossStoreRuntime
  ${REPO_ROOT}/src/activities/apps/applab
)
target_link_libraries(applab_runtime_tests PRIVATE crosspoint_test_common applab_lua ArduinoJson GTest::gtest_main)
gtest_discover_tests(applab_runtime_tests)
