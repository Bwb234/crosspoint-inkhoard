add_executable(InkHoardClientTest
  InkHoardClientTest.cpp
  ${REPO_ROOT}/lib/InkHoard/InkHoardClient.cpp
  ${REPO_ROOT}/lib/InkHoard/InkHoardTransport.cpp
  ${REPO_ROOT}/lib/InkHoard/InkHoardJsonParser.cpp
  ${REPO_ROOT}/lib/InkHoard/InkHoardModels.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(InkHoardClientTest PRIVATE
  ${REPO_ROOT}/lib/InkHoard
  ${REPO_ROOT}/lib/JsonParser
)

target_compile_definitions(InkHoardClientTest PRIVATE
  INKHOARD_FIXTURE_DIR="${REPO_ROOT}/test/fixtures/device-api"
)

target_link_libraries(InkHoardClientTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(InkHoardClientTest)
