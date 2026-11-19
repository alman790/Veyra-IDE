cmake_minimum_required(VERSION 3.20)
project(VeyraIDE VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    add_compile_options(/utf-8)
endif()

option(TS_BUILD_GUI "Build Qt 6 desktop application" ON)
option(TS_REQUIRE_GUI "Fail configuration if requested Qt GUI is unavailable" ON)
option(TS_BUILD_TESTS "Build unit and integration tests" ON)
option(TS_ENABLE_THREAD_SANITIZER "Instrument appropriate concurrent core tests with ThreadSanitizer" OFF)
option(TS_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(TS_ENABLE_THREAD_SANITIZER AND TS_ENABLE_SANITIZERS)
    message(FATAL_ERROR "Use separate builds for ThreadSanitizer and ASan/UBSan")
endif()
if(TS_ENABLE_THREAD_SANITIZER AND NOT MSVC)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()
if(TS_ENABLE_SANITIZERS AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()


find_package(Qt6 6.2 REQUIRED COMPONENTS Core)
add_executable(veyra_devtools tools/devtools.cpp)
target_link_libraries(veyra_devtools PRIVATE Qt6::Core)
add_custom_target(translations-check
    COMMAND $<TARGET_FILE:veyra_devtools> translations "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS veyra_devtools VERBATIM)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/languages.json")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/resources/languages.json" TS_LANGUAGE_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," TS_LANGUAGE_BYTES "${TS_LANGUAGE_HEX}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/core/LanguageCatalog.hpp.in" "${CMAKE_CURRENT_BINARY_DIR}/generated/LanguageCatalog.hpp" @ONLY)
find_package(Threads REQUIRED)

add_library(veyra_core STATIC
    src/infrastructure/AtomicFile.cpp
    src/core/Analyzer.cpp
    src/core/SourceMask.cpp
    src/core/Utf8.cpp
    src/core/LanguageTools.cpp
    src/core/ProjectTools.cpp
)
target_link_libraries(veyra_core PUBLIC Threads::Threads Qt6::Core)
target_include_directories(veyra_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(veyra_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(veyra_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(veyra_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(TS_LOCAL_MODEL "Build the in-process local code model" ON)
if(TS_LOCAL_MODEL)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
    set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
    set(GGML_METAL OFF CACHE BOOL "" FORCE)
    set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
    foreach(backend IN ITEMS CUDA WEBGPU OPENCL HEXAGON VIRTGPU)
        set(GGML_${backend} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(CMAKE_CXX_STANDARD 17)
    add_subdirectory(third_party/llama EXCLUDE_FROM_ALL)
    set(CMAKE_CXX_STANDARD 20)
    if(NOT MSVC)
        foreach(runtime_target IN ITEMS llama ggml ggml-base ggml-cpu ggml-blas)
            if(TARGET ${runtime_target})
                target_compile_options(${runtime_target} PRIVATE -O2)
            endif()
        endforeach()
    endif()

    add_library(veyra_local_model STATIC src/core/LocalCodeModel.cpp)
    target_include_directories(veyra_local_model PUBLIC include)
    target_link_libraries(veyra_local_model PUBLIC llama)
    target_compile_definitions(veyra_local_model PRIVATE TS_CODE_MODEL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/resources/models/code-model.gguf")
endif()

if(TS_BUILD_GUI)
    find_package(Qt6 6.2 QUIET COMPONENTS Widgets Concurrent
        PATHS /opt/homebrew/opt/qt /usr/local/opt/qt)
    if(NOT Qt6_FOUND)
        if(TS_REQUIRE_GUI)
            message(FATAL_ERROR "Qt6 Widgets >= 6.2 is required for the desktop application")
        endif()
        message(WARNING "Qt 6 Widgets SDK not available; building core only. Install Qt6 or set TS_BUILD_GUI=OFF.")
    endif()
    if(Qt6_FOUND)
        set(TS_GUI_SOURCES
            src/application/CMakeFileApi.cpp
            src/application/CMakeFileApi.hpp
            src/application/BackgroundJob.cpp
            src/application/BackgroundJob.hpp
            src/application/ProjectSearch.cpp
            src/application/ProjectSearch.hpp
            src/application/DocumentController.cpp
            src/application/DocumentController.hpp
            src/gui/WorkspaceControls.hpp
            src/gui/ProjectFileModel.hpp
            src/gui/WorkspaceWidgets.hpp
            src/gui/WorkspacePages.cpp
            src/gui/ThreadHints.cpp
            src/gui/LocalAssistant.cpp
            src/gui/LanguageActions.cpp
            src/gui/DebuggerPanel.cpp
            src/gui/DebugWorkspace.cpp
            src/gui/NewProjectDialog.cpp
            src/gui/SdkDiscovery.cpp
            src/gui/SdkInstaller.cpp
            src/application/ProjectCreator.cpp
            src/gui/SourceWorkspace.cpp
            src/gui/MainWindow.cpp
            src/gui/MainWindow.hpp
            src/gui/CodeEditor.cpp
            src/gui/CodeEditor.hpp
            src/platform/ProcessTree.cpp
            src/platform/ProcessTree.hpp
            src/gui/TerminalWidget.cpp
            src/gui/TerminalWidget.hpp
            src/gui/CodeRunner.cpp
            src/gui/CodeRunner.hpp
            src/gui/JsonRpcFramer.cpp
            src/gui/JsonRpcFramer.hpp
            src/gui/LspClient.cpp
            src/gui/LspClient.hpp
            src/gui/UiLayout.hpp
            src/gui/GlassSurfaces.hpp
            src/gui/UiIcons.hpp
            src/gui/ThemeManager.hpp
            src/gui/ThemeManager.cpp
            src/gui/Workspace.cpp
            src/gui/DocumentWorkspace.cpp
            src/gui/ProjectSearchDialog.cpp
            resources/branding.qrc
            src/gui/UiLanguage.cpp
            src/gui/UiLanguage.hpp
            src/gui/SourceInspector.cpp
            src/gui/SourceInspector.hpp
            src/gui/SettingsDialog.cpp
            src/gui/SettingsDialog.hpp
            src/gui/DesktopTools.cpp
            src/gui/GitPanel.cpp
            src/gui/GitPanel.hpp
            src/gui/MarkdownPreview.hpp
            src/gui/EditorTools.cpp
            src/gui/CMakeProjectDialog.cpp
            src/gui/CMakeProjectDialog.hpp
        )
        add_library(veyra_gui STATIC ${TS_GUI_SOURCES})
        set_target_properties(veyra_gui PROPERTIES AUTOMOC ON AUTORCC ON)
        target_link_libraries(veyra_gui PUBLIC veyra_core Qt6::Widgets Qt6::Concurrent)
        if(TS_LOCAL_MODEL)
            target_link_libraries(veyra_gui PUBLIC veyra_local_model)
            target_compile_definitions(veyra_gui PRIVATE TS_HAS_LOCAL_MODEL=1)
        else()
            target_compile_definitions(veyra_gui PRIVATE TS_HAS_LOCAL_MODEL=0)
        endif()

        find_package(SQLite3 QUIET)
        if(SQLite3_FOUND)
            if(TARGET SQLite3::SQLite3)
                target_link_libraries(veyra_gui PRIVATE SQLite3::SQLite3)
            else()
                target_link_libraries(veyra_gui PRIVATE SQLite::SQLite3)
            endif()
            target_compile_definitions(veyra_gui PRIVATE TS_HAS_SQLITE=1)
        else()
            target_compile_definitions(veyra_gui PRIVATE TS_HAS_SQLITE=0)
        endif()
        target_include_directories(veyra_gui PUBLIC src/gui)
        add_executable(VeyraIDE src/gui/main.cpp)
        target_link_libraries(VeyraIDE PRIVATE veyra_gui)
        add_custom_target(run
            COMMAND $<TARGET_FILE:VeyraIDE>
            DEPENDS VeyraIDE
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            USES_TERMINAL VERBATIM
            COMMENT "Build and run Veyra IDE")
        set_target_properties(VeyraIDE PROPERTIES
            WIN32_EXECUTABLE TRUE
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "Veyra IDE"
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.veyra.ide"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        )
        if(APPLE)
            target_sources(VeyraIDE PRIVATE resources/branding/VeyraIDE.icns)
            set_source_files_properties(resources/branding/VeyraIDE.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
            set_target_properties(VeyraIDE PROPERTIES MACOSX_BUNDLE_ICON_FILE "VeyraIDE.icns")
            set_target_properties(VeyraIDE PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/artifacts.noindex")
        endif()
        if(TARGET VeyraIDE)
            add_custom_target(deploy
                COMMAND ${CMAKE_COMMAND}
                    "-DTS_EXECUTABLE=$<TARGET_FILE:VeyraIDE>"
                    "-DTS_BUILD_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
                    "-DTS_CONFIGURATION=$<CONFIG>"
                    "-DTS_BUNDLE=$<IF:$<BOOL:${APPLE}>,$<TARGET_FILE_DIR:VeyraIDE>/../..,>"
                    "-DTS_QMAKE=$<TARGET_FILE:Qt6::qmake>"
                    "-DTS_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/deployment.noindex"
                    "-DTS_LICENSE_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/licenses"
                    "-DTS_LOCAL_MODEL=${TS_LOCAL_MODEL}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Deploy.cmake"
                DEPENDS VeyraIDE USES_TERMINAL VERBATIM)
        endif()
        if(APPLE)
            add_custom_target(installer COMMAND ${CMAKE_COMMAND} "-DTS_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/deployment.noindex" "-DTS_VERSION=${PROJECT_VERSION}" "-DTS_ARCH=${CMAKE_SYSTEM_PROCESSOR}" "-DTS_MIN_OS=${CMAKE_OSX_DEPLOYMENT_TARGET}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Installer.cmake" DEPENDS deploy USES_TERMINAL VERBATIM)
        endif()
        if(WIN32)
            enable_language(RC)
            set(TS_WINDOWS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/branding/VeyraIDE.ico")
            configure_file(resources/branding/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
            target_sources(VeyraIDE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
        endif()
        if(TS_LOCAL_MODEL)
            if(APPLE)
                set(TS_MODEL_OUTPUT "$<TARGET_FILE_DIR:VeyraIDE>/../Resources/models")
            else()
                set(TS_MODEL_OUTPUT "$<TARGET_FILE_DIR:VeyraIDE>/models")
            endif()
            add_custom_command(TARGET VeyraIDE POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${TS_MODEL_OUTPUT}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/resources/models/code-model.gguf" "${TS_MODEL_OUTPUT}/code-model.gguf"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/resources/models/manifest.json" "${TS_MODEL_OUTPUT}/manifest.json")
        endif()
        if(APPLE)
            set(TS_LEGACY_TOOL "$<TARGET_FILE_DIR:VeyraIDE>/../Resources/tools/debug_bridge.py")
        else()
            set(TS_LEGACY_TOOL "$<TARGET_FILE_DIR:VeyraIDE>/tools/debug_bridge.py")
        endif()
        add_custom_command(TARGET VeyraIDE POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f "${TS_LEGACY_TOOL}" VERBATIM)
        install(TARGETS VeyraIDE BUNDLE DESTINATION . RUNTIME DESTINATION bin)
        if(NOT APPLE)
            if(TS_LOCAL_MODEL)
                install(FILES resources/models/code-model.gguf resources/models/manifest.json
                    DESTINATION share/VeyraIDE/models)
            endif()
        endif()
        if(UNIX AND NOT APPLE)
            install(FILES resources/veyra.desktop DESTINATION share/applications)
            install(FILES resources/branding/veyra.png DESTINATION share/icons/hicolor/512x512/apps RENAME veyra.png)
        endif()
        if(MSVC)
            target_compile_options(veyra_gui PRIVATE /W4 /permissive-)
        else()
            target_compile_options(veyra_gui PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endif()
endif()

if(TS_BUILD_TESTS)
    enable_testing()
    if(TS_LOCAL_MODEL)
        add_executable(veyra_local_model_tests tests/local_model_tests.cpp)
        target_link_libraries(veyra_local_model_tests PRIVATE veyra_local_model)
        add_test(NAME local_model COMMAND veyra_local_model_tests)
        set_tests_properties(local_model PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    endif()

    add_executable(veyra_thread_hints_tests tests/thread_hints_tests.cpp)
    target_link_libraries(veyra_thread_hints_tests PRIVATE veyra_core)
    add_test(NAME thread_hints COMMAND veyra_thread_hints_tests)
    foreach(spec IN ITEMS "language;language_tools" "project;project_tools" "structures;structures_audit")
        list(GET spec 0 name)
        list(GET spec 1 source)
        add_executable(veyra_${name}_tests tests/${source}_tests.cpp)
        target_link_libraries(veyra_${name}_tests PRIVATE veyra_core)
        add_test(NAME ${name} COMMAND veyra_${name}_tests)
    endforeach()
    if(TARGET veyra_gui)
        find_package(Qt6 REQUIRED COMPONENTS Test)
        foreach(name IN ITEMS editor_tools document cmake_file_api lsp_integration process_integration ide_workflow debugger sdk_installer)
            add_executable(veyra_${name}_tests tests/${name}_tests.cpp)
            target_link_libraries(veyra_${name}_tests PRIVATE veyra_gui Qt6::Test)
            if(name STREQUAL "process_integration")
                add_test(NAME ${name} COMMAND veyra_${name}_tests $<TARGET_FILE:veyra_process_fixture>)
            else()
                add_test(NAME ${name} COMMAND veyra_${name}_tests)
            endif()
            set_tests_properties(${name} PROPERTIES TIMEOUT 150 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;TS_DISABLE_LOCAL_MODEL=1")
        endforeach()
        add_executable(veyra_process_fixture tests/process_fixture.cpp)
        target_link_libraries(veyra_process_fixture PRIVATE Qt6::Core)
        add_dependencies(veyra_process_integration_tests veyra_process_fixture)
        set_tests_properties(lsp_integration debugger PROPERTIES SKIP_RETURN_CODE 77)
        if(TS_LOCAL_MODEL)
            add_executable(veyra_assistant_workflow_tests tests/assistant_workflow_tests.cpp)
            target_link_libraries(veyra_assistant_workflow_tests PRIVATE veyra_gui Qt6::Test)
            add_test(NAME assistant_workflow COMMAND veyra_assistant_workflow_tests)
            set_tests_properties(assistant_workflow PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        endif()

    endif()
    get_property(TS_TEST_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    list(FILTER TS_TEST_TARGETS INCLUDE REGEX "^veyra_.*tests$")
    add_custom_target(check
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -C $<CONFIG>
        DEPENDS ${TS_TEST_TARGETS}
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        USES_TERMINAL VERBATIM)
endif()

install(DIRECTORY licenses/ DESTINATION share/VeyraIDE/licenses)
install(FILES LICENSE DESTINATION share/VeyraIDE)
set(CPACK_PACKAGE_NAME "VeyraIDE")
set(CPACK_PACKAGE_VENDOR "Veyra")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_GENERATOR "ZIP")
if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "TGZ")
    find_program(TS_DPKG dpkg)
    find_program(TS_RPMBUILD rpmbuild)
    find_program(TS_DPKG_SHLIBDEPS dpkg-shlibdeps)
    find_program(TS_FILE_TOOL file)
    if(TS_DPKG AND TS_DPKG_SHLIBDEPS AND TS_FILE_TOOL)
        list(APPEND CPACK_GENERATOR DEB)
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
        if(TARGET VeyraIDE)
            set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins")
        endif()
    endif()
    if(TS_RPMBUILD AND TS_FILE_TOOL)
        list(APPEND CPACK_GENERATOR RPM)
        set(CPACK_RPM_PACKAGE_AUTOREQ ON)
        if(TARGET VeyraIDE)
            set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtbase-gui")
        endif()
    endif()
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
endif()
set(CPACK_PACKAGE_CONTACT "Veyra IDE maintainers")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Veyra IDE C++ and Qt IDE")
if(WIN32)
    set(CPACK_INSTALL_CMAKE_PROJECTS "")
    set(CPACK_INSTALLED_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/deployment.noindex;.")
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
    set(CPACK_PACKAGE_EXECUTABLES "VeyraIDE;Veyra IDE")
    find_program(TS_MAKENSIS makensis HINTS "C:/Program Files (x86)/NSIS" "C:/Program Files/NSIS")
    if(TS_MAKENSIS)
        set(CPACK_GENERATOR "ZIP;NSIS")
    endif()
endif()
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/packages.noindex")
include(CPack)
if((WIN32 OR (UNIX AND NOT APPLE)) AND TARGET deploy)
    add_custom_target(installer COMMAND ${CMAKE_CPACK_COMMAND} --config "${CMAKE_CURRENT_BINARY_DIR}/CPackConfig.cmake" -C $<CONFIG> DEPENDS deploy USES_TERMINAL VERBATIM)
endif()

if(WIN32)
    target_link_libraries(veyra_core PUBLIC shell32)
endif()

find_program(TS_CLANG_FORMAT NAMES clang-format clang-format-22 clang-format-23
    HINTS "/Applications/CLion.app/Contents/plugins/clion-radler/DotFiles/macos-arm64"
          "/Applications/CLion.app/Contents/plugins/clion-radler/DotFiles/macos-x64")
if(TS_CLANG_FORMAT)
    add_custom_target(format COMMAND ${CMAKE_COMMAND}
        "-DTS_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DTS_CLANG_FORMAT=${TS_CLANG_FORMAT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Format.cmake" VERBATIM)
    add_custom_target(format-check COMMAND ${CMAKE_COMMAND}
        "-DTS_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DTS_CLANG_FORMAT=${TS_CLANG_FORMAT}"
        -DTS_FORMAT_CHECK=ON -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Format.cmake" VERBATIM)
endif()
