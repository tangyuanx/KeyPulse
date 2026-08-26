cmake_minimum_required(VERSION 3.21)
project(KeyPulse VERSION 0.1.0 LANGUAGES CXX)

add_executable(KeyPulse WIN32 src/main.cpp)
target_compile_features(KeyPulse PRIVATE cxx_std_20)
set_property(TARGET KeyPulse PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(KeyPulse PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KeyPulse PRIVATE /utf-8 /W4 /permissive- /EHsc)
target_link_options(KeyPulse PRIVATE /MANIFESTUAC:"level='asInvoker' uiAccess='false'")
target_link_libraries(KeyPulse PRIVATE gdiplus comdlg32 shell32 user32 gdi32 ole32)

install(TARGETS KeyPulse RUNTIME DESTINATION .)
