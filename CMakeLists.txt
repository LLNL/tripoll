# Copyright 2021-2022 Lawrence Livermore National Security, LLC and other
# TriPoll Project Developers. See the top-level COPYRIGHT file for details.
#
# SPDX-License-Identifier: MIT

# Works with 3.11 and tested through 3.15 (not tested yet)
cmake_minimum_required(VERSION 3.11...3.15)

project(TriPoll
    VERSION 0.1
	DESCRIPTION "Triangle Surveys with Metadata"
    LANGUAGES CXX)

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

if (NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
	message(STATUS "CMAKE_BUILD_TYPE is set as Release")
endif ()

add_subdirectory(src)
