cmake_minimum_required(VERSION 3.14)

catapult_library_target(catapult.ionet)
target_link_libraries(catapult.ionet catapult.model catapult.thread)
