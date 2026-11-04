cmake_minimum_required(VERSION 3.16)
project(switch-ime-term CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FT REQUIRED freetype2)
pkg_check_modules(FC REQUIRED fontconfig)
pkg_check_modules(PNG REQUIRED libpng)
pkg_check_modules(SDL2 sdl2)
pkg_check_modules(VTERM REQUIRED vterm)
pkg_check_modules(LUA REQUIRED lua5.4)

add_executable(switch-ime-term
    src/main.cpp
    src/platform/display_registry.cpp
    src/platform/soft_canvas.cpp
    src/theme/palette.cpp
    src/config/lua_config.cpp
    src/modules/terminal/vterm_parser.cpp
    src/platform/display_fbdev.cpp
    src/platform/display_headless.cpp
    src/render/font_manager.cpp
    src/render/glyph_atlas.cpp
    src/render/renderer.cpp
    src/term/pty.cpp
    src/term/osc_filter.cpp
    src/input/gamepad.cpp
    src/input/uinput_kbd.cpp
    src/input/evdev_gamepad.cpp
    src/input/uinput_mouse.cpp
    src/ime/pinyin_index.cpp
    src/ime/ngram.cpp
    src/ime/builtin_ime.cpp
    src/term/vterm_bridge.cpp
    src/ui/layout.cpp
    src/ui/layout_lua.cpp
    src/app/app.cpp
    src/app/ime_route.cpp
    src/app/physics.cpp
    src/ui/screen.cpp
    src/ui/panels.cpp
    src/ui/terminal_view.cpp
)

target_include_directories(switch-ime-term PRIVATE src ${LUA_INCLUDE_DIRS} ${FT_INCLUDE_DIRS} ${FC_INCLUDE_DIRS} ${VTERM_INCLUDE_DIRS})
if(SDL2_FOUND)
    target_sources(switch-ime-term PRIVATE src/platform/display_sdl.cpp)
    target_compile_definitions(switch-ime-term PRIVATE HAVE_SDL2)
    target_include_directories(switch-ime-term PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(switch-ime-term PRIVATE ${SDL2_LIBRARIES})
    target_link_directories(switch-ime-term PRIVATE ${SDL2_LIBRARY_DIRS})
endif()
target_link_libraries(switch-ime-term PRIVATE ${FT_LIBRARIES} ${FC_LIBRARIES} ${PNG_LIBRARIES} ${VTERM_LIBRARIES} ${LUA_LIBRARIES})
target_link_directories(switch-ime-term PRIVATE ${FT_LIBRARY_DIRS} ${FC_LIBRARY_DIRS} ${PNG_LIBRARY_DIRS} ${VTERM_LIBRARY_DIRS})
target_compile_options(switch-ime-term PRIVATE -Wall -Wextra -O2)
