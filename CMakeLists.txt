cmake_minimum_required(VERSION 3.20)
project(deckreader VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)
# sd-bus, for the keep-awake helper only (in the Freedesktop runtime).
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
# crengine-ng is built as a static library; its CMake package brings its
# own dependencies (FreeType, HarfBuzz, FriBiDi, libunibreak, zstd...).
find_package(crengine-ng REQUIRED CONFIG)
# Its exported target doesn't carry the header folder: look for it next to
# the package (lib/, lib64/ or lib/<triplet>/ layouts) and on the prefix path.
find_path(CRENGINE_INCLUDE_DIR crengine.h
    HINTS "${crengine-ng_DIR}/../../../include" "${crengine-ng_DIR}/../../../../include"
    PATH_SUFFIXES crengine-ng REQUIRED)

set(DECKREADER_CRENGINE_DATA "${CMAKE_INSTALL_FULL_DATADIR}/crengine-ng"
    CACHE PATH "Where crengine-ng's CSS and hyphenation patterns are installed")
configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h @ONLY)

# Everything except main.cpp, so the unit tests link the same code.
add_library(deckreader_core STATIC
    src/about.cpp
    src/app.cpp
    src/book.cpp
    src/canvas.cpp
    src/config.cpp
    src/display.cpp
    src/epubguard.cpp
    src/fsutil.cpp
    src/image_io.cpp
    src/input.cpp
    src/keepawake.cpp
    src/library.cpp
    src/library_screen.cpp
    src/log.cpp
    src/menu.cpp
    src/metacache.cpp
    src/reader_screen.cpp
    src/settings.cpp
    src/state.cpp
    src/system.cpp
    src/testscript.cpp
    src/text.cpp
    src/ui.cpp
    src/util.cpp
)
target_include_directories(deckreader_core PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}
    PRIVATE third_party)
target_include_directories(deckreader_core SYSTEM PUBLIC ${CRENGINE_INCLUDE_DIR})
target_link_libraries(deckreader_core PUBLIC
    crengine-ng::crengine-ng PkgConfig::SDL3 PkgConfig::FREETYPE PkgConfig::HARFBUZZ PkgConfig::PNG)
target_compile_options(deckreader_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

add_executable(deckreader src/main.cpp)
target_link_libraries(deckreader PRIVATE deckreader_core)
target_compile_options(deckreader PRIVATE -Wall -Wextra)

# Holds the screensaver inhibition while a book is read (D28); the app
# finds it next to its own executable.
add_executable(deckreader-keep-awake helper/deckreader-keep-awake.c)
target_link_libraries(deckreader-keep-awake PRIVATE PkgConfig::SYSTEMD)
target_compile_options(deckreader-keep-awake PRIVATE -Wall -Wextra)

install(TARGETS deckreader deckreader-keep-awake RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/config.ini DESTINATION ${CMAKE_INSTALL_DATADIR}/deckreader)

# Unit tests: pure logic (config, state, archive checks, sorting, input map).
option(DECKREADER_TESTS "Build the unit tests" ON)
if(DECKREADER_TESTS)
    add_executable(unit_tests tests/unit_tests.cpp)
    target_link_libraries(unit_tests PRIVATE deckreader_core)
    target_include_directories(unit_tests PRIVATE third_party)
    enable_testing()
    add_test(NAME unit_tests COMMAND unit_tests)
endif()
