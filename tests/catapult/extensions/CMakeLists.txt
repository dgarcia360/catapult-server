cmake_minimum_required(VERSION 3.14)

catapult_test_executable_target(tests.catapult.extensions local)
target_link_libraries(tests.catapult.extensions catapult.plugins.coresystem.deps tests.catapult.test.nemesis)
