cmake_minimum_required(VERSION 3.25)

project(Compresso VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Compresso is a Windows-only application.")
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "Compresso currently supports GCC/MinGW-w64 only.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)

add_executable(Compresso WIN32
    src/main.cpp
    src/main_window.cpp
    include/compresso/main_window.h
)

target_include_directories(Compresso PRIVATE include)
target_link_libraries(Compresso PRIVATE Qt6::Widgets)
target_compile_features(Compresso PRIVATE cxx_std_23)

install(TARGETS Compresso RUNTIME DESTINATION .)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)

    add_executable(CompressoUiTests
        tests/ui_tests.cpp
        src/main_window.cpp
        include/compresso/main_window.h
    )
    target_include_directories(CompressoUiTests PRIVATE include)
    target_link_libraries(CompressoUiTests PRIVATE Qt6::Widgets Qt6::Test)
    target_compile_features(CompressoUiTests PRIVATE cxx_std_23)

    add_test(NAME CompressoUiTests COMMAND CompressoUiTests)
    set_tests_properties(CompressoUiTests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()
