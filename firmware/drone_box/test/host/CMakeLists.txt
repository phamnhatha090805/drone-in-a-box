# Host unit tests for the hardware-free `command` component.
#
# This is a plain CMake project, NOT an ESP-IDF one: it compiles command.c with
# the host compiler and runs on your Mac in milliseconds, with no board, no
# flashing and no serial port. That is only possible because `command` includes
# no ESP-IDF headers -- keep it that way.
#
#   . ~/.espressif/tools/activate_idf_v6.0.2.sh    # only to locate Unity
#   cmake -S test/host -B build-host
#   cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
cmake_minimum_required(VERSION 3.22)
project(drone_box_host_tests C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Reuse the Unity that ships with ESP-IDF rather than vendoring a copy, so the
# host tests and any future on-target tests use the same framework.
set(UNITY_DIR "$ENV{IDF_PATH}/components/unity/unity/src")
if(NOT EXISTS "${UNITY_DIR}/unity.c")
    message(FATAL_ERROR
        "Unity not found at ${UNITY_DIR}\n"
        "Source the ESP-IDF activation script first so IDF_PATH is set:\n"
        "  . ~/.espressif/tools/activate_idf_v6.0.2.sh")
endif()

set(COMPONENTS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../components")

enable_testing()

# One executable per hardware-free component. Adding a component here is the
# whole cost of bringing it under host test -- which is the point of keeping
# ESP-IDF headers out of them.
function(host_test name)
    add_executable(test_${name}
        test_${name}.c
        "${COMPONENTS_DIR}/${name}/${name}.c"
        "${UNITY_DIR}/unity.c")

    target_include_directories(test_${name} PRIVATE
        "${COMPONENTS_DIR}/${name}/include"
        "${COMPONENTS_DIR}/board_config/include"
        "${UNITY_DIR}")

    # Components are built with -Werror on target; hold the host build to it too.
    target_compile_options(test_${name} PRIVATE -Wall -Wextra -Werror)

    add_test(NAME ${name} COMMAND test_${name})
endfunction()

host_test(command)
host_test(auth)
