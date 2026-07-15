file(GLOB XSTATE_TEST_SOURCES CONFIGURE_DEPENDS *.cpp adapters/*.cpp)
add_executable(xstate_tests ${XSTATE_TEST_SOURCES})
target_link_libraries(xstate_tests PRIVATE xstate::xstate)
target_include_directories(xstate_tests PRIVATE vendor)
# Real std headers instead of doctest's forward declarations: assertions that
# stringify std::shared_ptr need the full <ostream> on libstdc++ and MSVC STL.
target_compile_definitions(xstate_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(xstate_tests PRIVATE Threads::Threads)  # pthreads for posix adapter tests
endif()
add_test(NAME xstate_tests COMMAND xstate_tests)
