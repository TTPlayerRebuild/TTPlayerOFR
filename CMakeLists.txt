cmake_minimum_required(VERSION 3.24)
project(TtpOfr LANGUAGES C CXX RC)
set(TTP_PLUGIN_ID ttp_ofr)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "ttp_ofr requires MSVC and -A Win32")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(cmake/legacy_windows.cmake)
include(cmake/size_release.cmake)
include(cmake/codec.cmake)
include(cmake/version.cmake)
add_library(ttp_ofr SHARED src/plugin.cpp src/reader.cpp src/decoder.cpp
  src/ttp_ofr.def
  "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
add_dependencies(ttp_ofr ttp_ofr_version)
target_include_directories(ttp_ofr PUBLIC include PRIVATE src)
target_compile_definitions(ttp_ofr PRIVATE TTP_NAMESPACE=ofr UNICODE _UNICODE NOMINMAX
  WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0501 _CRT_SECURE_NO_WARNINGS OPENFROG_DYNAMIC_API)
target_compile_options(ttp_ofr PRIVATE /W4 /permissive- /EHsc /GR- /utf-8 /fp:precise /arch:SSE2)
target_link_libraries(ttp_ofr PRIVATE ttp_openfrog_backend ole32 uuid shlwapi)
target_link_options(ttp_ofr PRIVATE /ENTRY:DllMainCRTStartupForYY_Thunks
  /alternatename:_YY_ThunksOriginalDllMainCRTStartup@12=__DllMainCRTStartup@12)
set_target_properties(ttp_ofr PROPERTIES PREFIX "" OUTPUT_NAME "ttp_ofr")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(TARGET ttp_ofr POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_legacy_imports.py"
    "$<TARGET_FILE:ttp_ofr>"
    --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/5.1.2600.txt"
    --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/6.1.7600.txt"
    --report "$<TARGET_FILE_DIR:ttp_ofr>/legacy-imports.json" VERBATIM)
install(TARGETS ttp_ofr RUNTIME DESTINATION AddIn COMPONENT Runtime)
