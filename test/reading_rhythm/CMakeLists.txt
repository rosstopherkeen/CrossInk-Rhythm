add_executable(ReadingRhythmTest
  ReadingRhythmTest.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp
  ${REPO_ROOT}/src/activities/reader/rhythm/StatsSidecarFile.cpp
  ${REPO_ROOT}/src/activities/reader/rhythm/DailyMinutesLog.cpp
  ${REPO_ROOT}/src/activities/reader/rhythm/DailyMinutesBackfill.cpp
  ${REPO_ROOT}/src/activities/reader/rhythm/FinishedBooksIndex.cpp
  ${REPO_ROOT}/src/activities/reader/rhythm/BookDailyMinutes.cpp
  ${REPO_ROOT}/src/activities/reader/rhythm/BookRhythm.cpp
)

# Stubs first so HalStorage/HalClock/CrossPointSettings resolve to host fakes.
target_include_directories(ReadingRhythmTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)
target_include_directories(ReadingRhythmTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/lib/Memory
)

target_compile_definitions(ReadingRhythmTest PRIVATE READING_RHYTHM_TEST)

target_link_libraries(ReadingRhythmTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingRhythmTest)
