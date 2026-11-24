# msid-core: JUCE-free SID model, flusher, HTTP transport and MIDI engine
# (C++ port of sid.py / transport.py / engine.py).
add_library(msid-core STATIC
    src/Sid.cpp
    src/Flusher.cpp
    src/HttpTransport.cpp
    src/DmaSocketTransport.cpp
    src/Engine.cpp
    src/Presets.cpp
    src/Profile.cpp
    src/Toml.cpp
    src/Finder.cpp)
add_library(msid::core ALIAS msid-core)
target_include_directories(msid-core PUBLIC include)
target_compile_features(msid-core PUBLIC cxx_std_20)
set_target_properties(msid-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_package(Threads REQUIRED)
target_link_libraries(msid-core PUBLIC Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(msid-core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

if(MSID_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE
        SYSTEM)
    FetchContent_MakeAvailable(Catch2)

    add_executable(msid-core-tests
        tests/SidTests.cpp
        tests/FlusherTests.cpp
        tests/HttpTests.cpp
        tests/DmaTests.cpp
        tests/PresetsTests.cpp
        tests/ProfileTests.cpp
        tests/FinderTests.cpp
        tests/VectorTests.cpp)
    target_link_libraries(msid-core-tests PRIVATE msid::core Catch2::Catch2WithMain)
    target_compile_definitions(msid-core-tests PRIVATE
        MSID_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors"
        MSID_PRESETS_FILE="${CMAKE_SOURCE_DIR}/../presets.toml"
        MSID_PROFILES_DIR="${CMAKE_SOURCE_DIR}/../profiles"
        MSID_TEST_PROFILES_DIR="${CMAKE_SOURCE_DIR}/../tests/profiles")

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(msid-core-tests)
endif()
