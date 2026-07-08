# CpFontAdapter binds .cpfont metrics to FreeInkBook's RenderFont interface;
# the suite needs the engine's freestanding headers. Prefer the submodule;
# fall back to the sibling working checkout while the submodule pin predates
# libs/book (drop the fallback once the pin advances).
set(FREEINK_BOOK_INCLUDE "${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include")
if(NOT EXISTS "${FREEINK_BOOK_INCLUDE}")
  set(FREEINK_BOOK_INCLUDE "$ENV{HOME}/GitHub/freeink-sdk/libs/book/FreeInkBook/include")
endif()

if(NOT EXISTS "${FREEINK_BOOK_INCLUDE}")
  message(WARNING "FreeInkBook headers not found - skipping CpFontAdapterTest")
else()
  add_executable(CpFontAdapterTest
    CpFontAdapterTest.cpp
    ${REPO_ROOT}/src/activities/reader/CpFontAdapter.cpp
    ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
    ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
    ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  )

  target_include_directories(CpFontAdapterTest PRIVATE
    ${REPO_ROOT}/src/activities/reader
    ${REPO_ROOT}/lib/EpdFont
    ${REPO_ROOT}/lib/Utf8
    ${FREEINK_BOOK_INCLUDE}
  )

  target_link_libraries(CpFontAdapterTest PRIVATE
    crosspoint_test_common
    GTest::gtest_main
  )

  gtest_discover_tests(CpFontAdapterTest)
endif()
