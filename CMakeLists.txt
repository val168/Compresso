cmake_minimum_required(VERSION 3.25)

project(Compresso VERSION 1.0.0 LANGUAGES CXX)

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
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network Widgets)

add_executable(Compresso WIN32
    src/main.cpp
    src/operation_error.cpp
    src/compression_summary.cpp
    src/compression_backend.cpp
    src/filesystem_compression.cpp
    src/directory_validator.cpp
    src/directory_scanner.cpp
    src/operation_worker.cpp
    src/game_discovery.cpp
    src/theme_manager.cpp
    src/main_window.cpp
    resources/compresso.qrc
    include/compresso/directory_validator.h
    include/compresso/operation_error.h
    include/compresso/compression_summary.h
    include/compresso/compression_backend.h
    include/compresso/filesystem_compression.h
    include/compresso/directory_scanner.h
    include/compresso/operation_worker.h
    include/compresso/game_discovery.h
    include/compresso/theme_manager.h
    include/compresso/main_window.h
)

target_include_directories(Compresso PRIVATE include)
target_link_libraries(Compresso PRIVATE Qt6::Network Qt6::Widgets)
target_compile_features(Compresso PRIVATE cxx_std_23)
set_target_properties(Compresso PROPERTIES OUTPUT_NAME CompressoApp)
get_filename_component(COMPRESSO_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(QT_DEPLOY_BIN_DIR "dependencies")

# Make the build-tree executable runnable from VS Code, Explorer, or a terminal
# by placing its Qt and MinGW runtime dependencies beside it after each build.
add_custom_command(TARGET Compresso POST_BUILD
    COMMAND "${WINDEPLOYQT_EXECUTABLE}"
            --compiler-runtime
            --no-translations
            --dir "$<TARGET_FILE_DIR:Compresso>"
            "$<TARGET_FILE:Compresso>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${COMPRESSO_COMPILER_BIN}/libgcc_s_seh-1.dll"
            "$<TARGET_FILE_DIR:Compresso>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${COMPRESSO_COMPILER_BIN}/libstdc++-6.dll"
            "$<TARGET_FILE_DIR:Compresso>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${COMPRESSO_COMPILER_BIN}/libwinpthread-1.dll"
            "$<TARGET_FILE_DIR:Compresso>"
    COMMAND "${CMAKE_COMMAND}"
            "-DCOMPRESSO_DEPLOY_ROOT=$<TARGET_FILE_DIR:Compresso>"
            "-DCOMPRESSO_MAIN_EXECUTABLE=$<TARGET_FILE:Compresso>"
            -P "${CMAKE_CURRENT_BINARY_DIR}/DeployRuntimeDependencies.cmake"
    COMMENT "Deploying Qt and MinGW runtime dependencies beside CompressoApp.exe"
    VERBATIM
)

# Keep the user-facing executable free of Qt imports. Windows resolves an
# executable's imported DLLs before its entry point, so a small launcher starts
# the Qt application from the adjacent dependencies directory.
add_executable(CompressoLauncher WIN32 src/launcher.cpp)
target_compile_features(CompressoLauncher PRIVATE cxx_std_23)
target_link_options(CompressoLauncher PRIVATE -municode -static -static-libgcc -static-libstdc++)
set_target_properties(CompressoLauncher PROPERTIES OUTPUT_NAME Compresso)

install(TARGETS CompressoLauncher RUNTIME DESTINATION .)
install(TARGETS Compresso RUNTIME DESTINATION dependencies)
install(FILES LICENSE resources/PORTABLE-README.txt DESTINATION .)

qt_generate_deploy_app_script(
    TARGET Compresso
    OUTPUT_SCRIPT COMPRESSO_DEPLOY_SCRIPT
    NO_TRANSLATIONS
    DEPLOY_TOOL_OPTIONS --compiler-runtime --skip-plugin-types
                         generic,networkinformation
)
install(CODE "set(QT_DEPLOY_BIN_DIR \"dependencies\")")
install(SCRIPT "${COMPRESSO_DEPLOY_SCRIPT}")

# windeployqt deploys Qt libraries and plugins. Resolve their non-system DLL
# dependencies as well, so the portable directory is independent of PATH.
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
        DEPENDS Compresso CompressoLauncher
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
        src/filesystem_compression.cpp
        src/directory_validator.cpp
        src/directory_scanner.cpp
        src/operation_worker.cpp
        src/game_discovery.cpp
        src/theme_manager.cpp
        src/main_window.cpp
        include/compresso/directory_validator.h
        include/compresso/operation_error.h
        include/compresso/compression_summary.h
        include/compresso/compression_backend.h
        include/compresso/filesystem_compression.h
        include/compresso/directory_scanner.h
        include/compresso/operation_worker.h
        include/compresso/game_discovery.h
        include/compresso/theme_manager.h
        include/compresso/main_window.h
    )
    target_include_directories(CompressoUiTests PRIVATE include)
    target_link_libraries(CompressoUiTests PRIVATE Qt6::Network Qt6::Widgets Qt6::Test)
    target_compile_features(CompressoUiTests PRIVATE cxx_std_23)

    add_custom_command(TARGET CompressoUiTests POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --compiler-runtime
                --no-translations
                --dir "$<TARGET_FILE_DIR:CompressoUiTests>"
                "$<TARGET_FILE:CompressoUiTests>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${COMPRESSO_COMPILER_BIN}/libgcc_s_seh-1.dll"
                "$<TARGET_FILE_DIR:CompressoUiTests>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${COMPRESSO_COMPILER_BIN}/libstdc++-6.dll"
                "$<TARGET_FILE_DIR:CompressoUiTests>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${COMPRESSO_COMPILER_BIN}/libwinpthread-1.dll"
                "$<TARGET_FILE_DIR:CompressoUiTests>"
        COMMAND "${CMAKE_COMMAND}"
                "-DCOMPRESSO_DEPLOY_ROOT=$<TARGET_FILE_DIR:CompressoUiTests>"
                "-DCOMPRESSO_MAIN_EXECUTABLE=$<TARGET_FILE:CompressoUiTests>"
                -P "${CMAKE_CURRENT_BINARY_DIR}/DeployRuntimeDependencies.cmake"
        COMMENT "Deploying Qt and MinGW runtime dependencies beside CompressoUiTests.exe"
        VERBATIM
    )

    add_test(NAME CompressoUiTests
        COMMAND CompressoUiTests
                -o "${CMAKE_CURRENT_BINARY_DIR}/CompressoUiTests.txt,txt"
                -v1
    )
    set_tests_properties(CompressoUiTests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${CMAKE_PREFIX_PATH}/share/qt6/plugins"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${CMAKE_PREFIX_PATH}/bin"
    )
endif()
