FetchContent_Declare(
  arduinojson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(arduinojson)

add_executable(hermes_protocol_tests
  HermesProtocolTest.cpp
  ${REPO_ROOT}/src/activities/apps/hermes/HermesProtocol.cpp
)
target_include_directories(hermes_protocol_tests PRIVATE ${REPO_ROOT}/src/activities/apps/hermes)
target_link_libraries(hermes_protocol_tests PRIVATE crosspoint_test_common ArduinoJson GTest::gtest_main)
gtest_discover_tests(hermes_protocol_tests)
