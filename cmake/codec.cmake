# Private, source-only snapshot: no proprietary SDK, import library, external
# OpenFROG.dll, resource payload or runtime extraction. The top-level plugin
# owns the one XP CRT/YY-Thunks entry point; do not add the upstream DLL target.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_openfrog "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openfrog")
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_LIST_DIR}/check_openfrog_source.py" "${_openfrog}"
  COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_openfrog}/SOURCE_MANIFEST.json")
add_library(ttp_openfrog_backend STATIC
  "${_openfrog}/src/api.cpp" "${_openfrog}/src/container.cpp"
  "${_openfrog}/src/entropy.cpp" "${_openfrog}/src/pcm.cpp"
  "${_openfrog}/src/codec.cpp" "${_openfrog}/src/entropy3.cpp"
  "${_openfrog}/src/mapping.cpp" "${_openfrog}/src/predictor.cpp"
  "${_openfrog}/src/predictor_lpc.cpp" "${_openfrog}/src/predictor_cascade.cpp")
target_include_directories(ttp_openfrog_backend PUBLIC "${_openfrog}/include"
  PRIVATE "${_openfrog}/src")
# The upstream header's BUILD+USE_DEF combination declares plain C functions,
# without exporting them from ttp_ofr. Callers use DYNAMIC_API to suppress
# dllimport as well; here their references are resolved by this static target.
target_compile_definitions(ttp_openfrog_backend PRIVATE OPENFROG_BUILD OPENFROG_USE_DEF
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0501)
target_compile_options(ttp_openfrog_backend PRIVATE /W4 /WX /EHsc /utf-8 /fp:strict /arch:SSE2)
