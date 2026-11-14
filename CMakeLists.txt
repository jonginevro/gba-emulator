cmake_minimum_required(VERSION 3.16)
project(gba C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
	set(SDL2_TARGET PkgConfig::SDL2)
else()
	set(SDL2_TARGET SDL2::SDL2)
endif()

file(GLOB SOURCES ${CMAKE_SOURCE_DIR}/src/*.c)
add_executable(gba ${SOURCES})
target_link_libraries(gba PRIVATE ${SDL2_TARGET} m)
target_compile_options(gba PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -fno-common -fno-strict-aliasing)
