cmake_minimum_required(VERSION 2.8)
project(scorep_substrate_graphviz)

option(BUILD_DEBUG "Include debugging symbols in library and print some usefull output on execution." OFF)
option(DYNAMIC_ID "Generate a dynamic stack ID to distinguish calls." ON)
option(OPENMP_MPI "Generate graphviz files for exclusively running functions." OFF)

set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/common)

find_package(Scorep REQUIRED)

set(PLUGIN_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/graphviz.c)

add_definitions("-Wall -Wextra -pedantic -std=c11")

if(OPENMP_MPI)
	add_definitions("-DONLY_ENTER")
endif()

if(DYNAMIC_ID)
	add_definitions("-DCREATE_DYN_ID")
endif()

if(BUILD_DEBUG)
	add_definitions("-ggdb")
endif()

include_directories(${PROJECT_SOURCE_DIR}/src/ ${SCOREP_INCLUDE_DIRS})
add_library(${PROJECT_NAME} SHARED ${PLUGIN_SOURCE_FILES})
