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

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets)

add_executable(Compresso WIN32
    src/main.cpp
    src/operation_error.cpp
    src/compression_summary.cpp
    src/compression_backend.cpp
    src/directory_validator.cpp
    src/directory_scanner.cpp
    src/operation_worker.cpp
    src/theme_manager.cpp
    src/main_window.cpp
    include/compresso/directory_validator.h
    include/compresso/operation_error.h
    include/compresso/compression_summary.h
    include/compresso/compression_backend.h
    include/compresso/directory_scanner.h
    include/compresso/operation_worker.h
    include/compresso/theme_manager.h
    include/compresso/main_window.h
)

target_include_directories(Compresso PRIVATE include)
target_link_libraries(Compresso PRIVATE Qt6::Widgets)
target_compile_features(Compresso PRIVATE cxx_std_23)

install(TARGETS Compresso RUNTIME DESTINATION bin)
install(FILES LICENSE resources/PORTABLE-README.txt DESTINATION .)

qt_generate_deploy_app_script(
    TARGET Compresso
    OUTPUT_SCRIPT COMPRESSO_DEPLOY_SCRIPT
    NO_TRANSLATIONS
    DEPLOY_TOOL_OPTIONS --compiler-runtime
)
install(SCRIPT "${COMPRESSO_DEPLOY_SCRIPT}")

# windeployqt deploys Qt libraries and plugins. Resolve their non-system DLL
# dependencies as well, so the portable directory is independent of PATH.
get_filename_component(COMPRESSO_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(COMPRESSO_QT_BIN "${WINDEPLOYQT_EXECUTABLE}" DIRECTORY)
find_program(COMPRESSO_OBJDUMP NAMES objdump REQUIRED)
configure_file(
    cmake/DeployRuntimeDependencies.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/DeployRuntimeDependencies.cmake"
    @ONLY
)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/DeployRuntimeDependencies.cmake")

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(COMPRESSO_PORTABLE_ROOT "${CMAKE_BINARY_DIR}/portable")
    set(COMPRESSO_PORTABLE_DIR "${COMPRESSO_PORTABLE_ROOT}/Compresso")
    set(COMPRESSO_PORTABLE_ARCHIVE
        "${COMPRESSO_PORTABLE_ROOT}/Compresso-${PROJECT_VERSION}-windows-x64.zip")

    add_custom_target(package-portable
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${COMPRESSO_PORTABLE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${COMPRESSO_PORTABLE_ARCHIVE}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${COMPRESSO_PORTABLE_ROOT}"
        COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
                --prefix "${COMPRESSO_PORTABLE_DIR}" --config "$<CONFIG>"
        COMMAND "${CMAKE_COMMAND}" -E chdir "${COMPRESSO_PORTABLE_ROOT}"
                "${CMAKE_COMMAND}" -E tar cf "${COMPRESSO_PORTABLE_ARCHIVE}"
                --format=zip Compresso
        DEPENDS Compresso
        COMMENT "Deploying Qt runtime and creating the portable Compresso ZIP"
        VERBATIM
    )
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)

    add_executable(CompressoUiTests
        tests/ui_tests.cpp
        src/operation_error.cpp
        src/compression_summary.cpp
        src/compression_backend.cpp
        src/directory_validator.cpp
        src/directory_scanner.cpp
        src/operation_worker.cpp
        src/theme_manager.cpp
        src/main_window.cpp
        include/compresso/directory_validator.h
        include/compresso/operation_error.h
        include/compresso/compression_summary.h
        include/compresso/compression_backend.h
        include/compresso/directory_scanner.h
        include/compresso/operation_worker.h
        include/compresso/theme_manager.h
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
