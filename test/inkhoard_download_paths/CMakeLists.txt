add_executable(InkHoardDownloadPathsTest
  InkHoardDownloadPathsTest.cpp
  ${REPO_ROOT}/lib/InkHoard/InkHoardDownloadPaths.cpp
  ${REPO_ROOT}/lib/InkHoard/InkHoardModels.cpp
)

target_include_directories(InkHoardDownloadPathsTest PRIVATE
  ${REPO_ROOT}/lib/InkHoard
)

target_link_libraries(InkHoardDownloadPathsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(InkHoardDownloadPathsTest)
