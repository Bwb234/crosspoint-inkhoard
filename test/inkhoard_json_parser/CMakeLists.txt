add_executable(InkHoardJsonParserTest
  InkHoardJsonParserTest.cpp
  ${REPO_ROOT}/lib/InkHoard/InkHoardJsonParser.cpp
  ${REPO_ROOT}/lib/InkHoard/InkHoardModels.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(InkHoardJsonParserTest PRIVATE
  ${REPO_ROOT}/lib/InkHoard
  ${REPO_ROOT}/lib/JsonParser
)

target_compile_definitions(InkHoardJsonParserTest PRIVATE
  INKHOARD_FIXTURE_DIR="${REPO_ROOT}/test/fixtures/device-api"
)

target_link_libraries(InkHoardJsonParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(InkHoardJsonParserTest)
