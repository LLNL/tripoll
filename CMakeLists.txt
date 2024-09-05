# Copyright 2021-2022 Lawrence Livermore National Security, LLC and other
# TriPoll Project Developers. See the top-level COPYRIGHT file for details.
#
# SPDX-License-Identifier: MIT

# Works with 3.11 and tested through 3.15 (not tested yet)
cmake_minimum_required(VERSION 3.11)

project(TriPoll
    VERSION 0.1
	DESCRIPTION "Triangle Surveys with Metadata"
    LANGUAGES CXX)

if (CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  set(TRIPOLL_MAIN_PROJECT ON)
endif ()

# Only do these if this is the main project, and not if it is included through
# add_subdirectory
if (TRIPOLL_MAIN_PROJECT)

    # Let's nicely support folders in IDE's
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)

    # Testing only available if this is the main app Note this needs to be done
    # in the main CMakeLists since it calls enable_testing, which must be in the
    # main CMakeLists.
    include(CTest)
endif ()

# Require out-of-source builds
file(TO_CMAKE_PATH "${PROJECT_BINARY_DIR}/CMakeLists.txt" LOC_PATH)
if (EXISTS "${LOC_PATH}")
    message(
        FATAL_ERROR
            "You cannot build in a source directory (or any directory with a CMakeLists.txt file). Please make a build subdirectory. Feel free to remove CMakeCache.txt and CMakeFiles."
    )
endif ()

include(FetchContent)

#
#  MPI
find_package(MPI)

# 
#  cereal 
find_package(cereal QUIET) 
if (NOT cereal_FOUND)     
	set(JUST_INSTALL_CEREAL TRUE)
	FetchContent_Declare(cereal             
		GIT_REPOSITORY https://github.com/USCiLab/cereal.git             
		GIT_TAG master             
		)     
	FetchContent_MakeAvailable(cereal) 
endif ()

#
#  ygm
find_package(ygm CONFIG)
if(NOT ygm_FOUND)
	FetchContent_Declare(
	ygm
	GIT_REPOSITORY https://github.com/LLNL/ygm.git
	GIT_TAG        develop         
	)         
	set(JUST_INSTALL_YGM ON)
	FetchContent_MakeAvailable(ygm)
	message(STATUS "Cloned ygm dependency " ${ygm_SOURCE_DIR})
else()
message(STATUS "Found ygm dependency " ${ygm_DIR})
endif()

function(setup_ygm_target exe_name)
	add_executable(${exe_name} ${exe_name}.cpp)
	target_link_libraries(${exe_name} PRIVATE ygm::ygm)
	message(STATUS "Adding include directory: " ${CMAKE_SOURCE_DIR}/include)
	target_include_directories(${exe_name} PUBLIC ${CMAKE_SOURCE_DIR}/include)
endfunction()

#
# Create the Tripoll target library
#
add_library(tripoll INTERFACE)
add_library(tripoll::tripoll ALIAS tripoll)
target_compile_features(tripoll INTERFACE cxx_std_20)
target_include_directories(
  tripoll INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                    $<INSTALL_INTERFACE:include>
)
target_link_libraries(tripoll INTERFACE ygm
)

option(JUST_INSTALL_TRIPOLL "Skip executable compilation" OFF)
if (JUST_INSTALL_TRIPOLL)
    return()
endif ()

if (NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
	message(STATUS "CMAKE_BUILD_TYPE is set as Release")
endif ()

if (TRIPOLL_MAIN_PROJECT)
  add_subdirectory(src)
endif()
