cmake_minimum_required(VERSION 3.16)
project(ClaudeUsageMonitor CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
if(MSVC)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/W4> $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT
endif()

add_library(usage_client STATIC src/UsageClient.cpp)
target_link_libraries(usage_client PUBLIC winhttp)

add_executable(ClaudeUsageMonitor WIN32
  src/main.cpp src/TaskbarWidget.cpp src/MainWindow.cpp src/OptionsDialog.cpp
  src/Canvas.cpp src/UsageView.cpp src/UsagePoller.cpp src/UsageIcon.cpp src/Settings.cpp res/app.rc)
target_link_libraries(ClaudeUsageMonitor PRIVATE usage_client shell32 user32 gdi32 comctl32 comdlg32 advapi32)
if(MSVC)
  target_link_options(ClaudeUsageMonitor PRIVATE /MANIFEST:NO)  # manifest comes from app.rc
endif()

add_executable(claude-usage src/cli.cpp)
target_link_libraries(claude-usage PRIVATE usage_client)
