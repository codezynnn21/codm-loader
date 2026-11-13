cmake_minimum_required(VERSION 3.22)
project(codm_loader C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
FetchContent_Declare(dobby
    GIT_REPOSITORY https://github.com/jmpews/Dobby.git
    GIT_TAG master
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(dobby)

if(TARGET dobby_static)
    set(DOBBY_LIB dobby_static)
else()
    set(DOBBY_LIB dobby)
endif()

add_executable(injector injector/main.c)
target_compile_options(injector PRIVATE -Wall -Wextra)
target_link_libraries(injector PRIVATE log dl)
# lands in app/src/main/assets so gradle packages it; MainActivity extracts + execs
set_target_properties(injector PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../assets")

add_library(codmhook SHARED hook/hook.cpp)
target_compile_options(codmhook PRIVATE -Wall -Wextra -fvisibility=hidden)
target_link_libraries(codmhook PRIVATE ${DOBBY_LIB} log dl)
