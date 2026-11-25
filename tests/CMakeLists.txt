# Every tests/test_*.cpp is one test binary, registered with CTest.
file(GLOB ME_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
foreach(src ${ME_TEST_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE me_core)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(${name} PRIVATE
    ME_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    ME_BINARY_DIR="${PROJECT_BINARY_DIR}")
  add_test(NAME ${name} COMMAND ${name})
endforeach()
