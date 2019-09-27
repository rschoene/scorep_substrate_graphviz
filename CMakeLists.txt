cmake_minimum_required(VERSION 2.8)
project(scorep_substrate_graphviz)

option(BUILD_DEBUG "Include debugging symbols in library and print some usefull output on execution." OFF)
option(DYNAMIC_ID "Generate a dynamic stack ID to distinguish calls." ON)
option(OPENMP_MPI "Generate graphviz files for exclusively running functions." OFF)
option(BACKEND_SCOREP "Build plugin using scorep(ON) or vampirtrace(OFF)" ON)

set(SCOREP_FOUND false)

set(PLUGIN_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/graphviz.c)

add_definitions("-Wall -Wextra -pedantic -std=c11")

include(common/FindScorep.cmake)
if(SCOREP_FOUND)
    include_directories(${SCOREP_INCLUDE_DIRS})
    add_definitions("-DSCOREP")
else()
    message(SEND_ERROR "Score-P was not found")
endif()

if(OPENMP_MPI)
	add_definitions("-DONLY_ENTER")
endif()

if(DYNAMIC_ID)
	add_definitions("-DCREATE_DYN_ID")
endif()

if(BUILD_DEBUG)
	add_definitions("-ggdb")
endif()

include_directories(${PROJECT_SOURCE_DIR}/src/ ${PROJECT_SOURCE_DIR}/build/scorep_substrate_graphviz/include)

add_library(${PROJECT_NAME} SHARED ${PLUGIN_SOURCE_FILES})
