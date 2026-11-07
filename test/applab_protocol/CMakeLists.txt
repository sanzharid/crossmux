# ArduinoJson is fetched by hermes_protocol, which test/CMakeLists.txt adds first.
add_executable(applab_protocol_tests
  AppLabProtocolTest.cpp
  ${REPO_ROOT}/src/activities/apps/applab/AppLabProtocol.cpp
)
target_include_directories(applab_protocol_tests PRIVATE ${REPO_ROOT}/src/activities/apps/applab)
target_link_libraries(applab_protocol_tests PRIVATE crosspoint_test_common ArduinoJson GTest::gtest_main)
gtest_discover_tests(applab_protocol_tests)
