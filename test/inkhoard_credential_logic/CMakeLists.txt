add_executable(InkHoardCredentialLogicTest
  InkHoardCredentialLogicTest.cpp
  ${REPO_ROOT}/lib/InkHoard/InkHoardCredentialLogic.cpp
)

target_include_directories(InkHoardCredentialLogicTest PRIVATE
  ${REPO_ROOT}/lib/InkHoard
)

target_link_libraries(InkHoardCredentialLogicTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(InkHoardCredentialLogicTest)
