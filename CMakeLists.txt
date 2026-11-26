cmake_minimum_required(VERSION 3.18)
project(HeadlightTuner CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Path to the AML "include" folder (the one that contains mod/amlmod.h)
set(AML_INCLUDE "" CACHE PATH "AML include directory")

# Take every .cpp file that lies next to this CMakeLists.txt (so the file name does not matter)
file(GLOB PLUGIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
if(NOT PLUGIN_SOURCES)
  message(FATAL_ERROR "No .cpp file next to CMakeLists.txt. Put main.cpp in the repository root.")
endif()

# AML's Logger and Config classes are implemented in .cpp files that every mod compiles in itself
set(AML_SOURCES "")
foreach(f logger.cpp config.cpp)
  if(EXISTS "${AML_INCLUDE}/mod/${f}")
    list(APPEND AML_SOURCES "${AML_INCLUDE}/mod/${f}")
  else()
    message(FATAL_ERROR "AML file not found: ${AML_INCLUDE}/mod/${f}")
  endif()
endforeach()
message(STATUS "Sources: ${PLUGIN_SOURCES} ${AML_SOURCES}")

add_library(HeadlightTuner SHARED ${PLUGIN_SOURCES} ${AML_SOURCES})
target_include_directories(HeadlightTuner PRIVATE ${AML_INCLUDE})
target_compile_options(HeadlightTuner PRIVATE -O2 -Wall)
target_link_libraries(HeadlightTuner PRIVATE log dl)
