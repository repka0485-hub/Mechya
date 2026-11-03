cmake_minimum_required(VERSION 3.18)
project(esp CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden -O2")

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/imgui CACHE PATH "")

add_definitions(-DIMGUI_IMPL_OPENGL_ES3)
include_directories(${IMGUI_DIR} ${IMGUI_DIR}/backends ${CMAKE_SOURCE_DIR})
add_library(esp SHARED main.cpp And64InlineHook.cpp
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_link_libraries(esp log EGL GLESv3 dl)
