cmake_minimum_required(VERSION 3.25.0)
project(MetasequoiaImePortableTests LANGUAGES CXX)

# A deliberately tiny core target holds parsers without Windows APIs. It needs no vcpkg or Boost,
# so CI can run it under AddressSanitizer and UndefinedBehaviorSanitizer on a Linux runner in
# seconds. The companion TLS target links OpenSSL; Windows pairing tests also need nlohmann/json.
# The full server test executable links Boost, curl, sqlite, WebView2 and D2D,
# and a sanitized build of that chain is a long job on a Windows runner.
#
# What belongs here: host-independent logic that reads data the user did not write -- skin packages,
# dictionary files, IPC payloads, config. Add sources as they become separable; anything needing a
# Windows header stays in the main test executable next door.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SERVER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(portable_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${SERVER_ROOT}/tests/src/test_skin_css_policy.cpp"
    "${SERVER_ROOT}/tests/src/test_input_key_policy.cpp"
    "${SERVER_ROOT}/tests/src/test_candidate_view_model.cpp"
    "${SERVER_ROOT}/tests/src/test_candidate_text_policy.cpp"
    "${SERVER_ROOT}/tests/src/test_clipboard_record.cpp"
    "${SERVER_ROOT}/tests/src/test_clipboard_sync.cpp"
    "${SERVER_ROOT}/src/sync/clipboard_receive_policy.cpp"
    "${SERVER_ROOT}/src/clipboard/clipboard_record.cpp"
    "${SERVER_ROOT}/src/webview2/skin_css_policy.cpp"
)

target_include_directories(portable_tests PRIVATE "${SERVER_ROOT}" "${SERVER_ROOT}/src" "${SERVER_ROOT}/..")

if(MSIME_PORTABLE_TESTS_SANITIZE AND NOT MSVC)
    target_compile_options(portable_tests PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
    target_link_options(portable_tests PRIVATE -fsanitize=address,undefined)
endif()

if(MSVC)
    # Standalone project, so the server's directory-wide /utf-8 does not reach it.
    target_compile_options(portable_tests PRIVATE /utf-8)
else()
    target_compile_options(portable_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

include(CTest)
add_test(NAME portable_tests COMMAND portable_tests)

# Real socket/TLS tests use the same OpenSSL implementation as production, without engine data.
find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)
add_executable(clipboard_tls_tests main.cpp
    "${SERVER_ROOT}/tests/src/test_clipboard_tls.cpp"
    "${SERVER_ROOT}/src/sync/clipboard_tls.cpp"
    "${SERVER_ROOT}/src/clipboard/clipboard_record.cpp")
target_include_directories(clipboard_tls_tests PRIVATE "${SERVER_ROOT}" "${SERVER_ROOT}/src" "${SERVER_ROOT}/..")
target_link_libraries(clipboard_tls_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(clipboard_tls_tests PRIVATE ws2_32)
endif()
if(MSVC)
    target_compile_options(clipboard_tls_tests PRIVATE /utf-8)
else()
    target_compile_options(clipboard_tls_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(MSIME_PORTABLE_TESTS_SANITIZE)
        target_compile_options(clipboard_tls_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
        target_link_options(clipboard_tls_tests PRIVATE -fsanitize=address,undefined)
    endif()
endif()
add_test(NAME clipboard_tls_tests COMMAND clipboard_tls_tests)
set_tests_properties(clipboard_tls_tests PROPERTIES TIMEOUT 30)

# Run the real Windows history read/write path without dictionaries, Boost or WebView2.
if(WIN32)
    add_executable(clipboard_history_tests
        main.cpp clipboard_history_paths.cpp
        "${SERVER_ROOT}/tests/src/test_clipboard_history.cpp"
        "${SERVER_ROOT}/src/clipboard/clipboard_history.cpp"
        "${SERVER_ROOT}/src/sync/clipboard_receive_policy.cpp"
        "${SERVER_ROOT}/src/clipboard/clipboard_record.cpp")
    target_include_directories(clipboard_history_tests PRIVATE "${SERVER_ROOT}" "${SERVER_ROOT}/src" "${SERVER_ROOT}/..")
    target_link_libraries(clipboard_history_tests PRIVATE user32)
    if(MSVC)
        target_compile_options(clipboard_history_tests PRIVATE /utf-8)
    endif()
    add_test(NAME clipboard_history_tests COMMAND clipboard_history_tests)

    find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)
    add_executable(clipboard_pairing_tests main.cpp clipboard_history_paths.cpp
        "${SERVER_ROOT}/tests/src/test_clipboard_pairing.cpp"
        "${SERVER_ROOT}/src/sync/clipboard_pairing.cpp"
        "${SERVER_ROOT}/src/sync/clipboard_sync_service.cpp"
        "${SERVER_ROOT}/src/sync/clipboard_tls.cpp"
        "${SERVER_ROOT}/src/config/credential_protection.cpp"
        "${SERVER_ROOT}/src/clipboard/clipboard_history.cpp"
        "${SERVER_ROOT}/src/clipboard/clipboard_record.cpp"
        "${SERVER_ROOT}/src/sync/clipboard_receive_policy.cpp")
    target_include_directories(clipboard_pairing_tests PRIVATE "${SERVER_ROOT}" "${SERVER_ROOT}/src"
        "${SERVER_ROOT}/.." "${NLOHMANN_JSON_INCLUDE_DIR}")
    target_link_libraries(clipboard_pairing_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto ws2_32 crypt32 user32)
    target_compile_options(clipboard_pairing_tests PRIVATE /utf-8)
    add_test(NAME clipboard_pairing_tests COMMAND clipboard_pairing_tests)
    set_tests_properties(clipboard_pairing_tests PROPERTIES TIMEOUT 30)
endif()
