add_executable(ReactionDiffusionTest
  ReactionDiffusionTest.cpp
  ${REPO_ROOT}/src/util/ReactionDiffusion.cpp
)

target_include_directories(ReactionDiffusionTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(ReactionDiffusionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReactionDiffusionTest)
