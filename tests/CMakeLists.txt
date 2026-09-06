# Tests. The harness has no timeouts; a hanging test is a defect.
# Windows: /W4 /WX applies to first-party test code via this helper.
function(tg_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE TailGovernor::tailgovernor)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /WX)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

tg_add_test(tg_test_core test_core.cpp)
tg_add_test(tg_test_governor test_governor.cpp)
tg_add_test(tg_test_authority test_authority.cpp)
tg_add_test(tg_test_recovery test_recovery.cpp)
tg_add_test(tg_test_persistence test_persistence.cpp)
tg_add_test(tg_test_features test_features.cpp)
tg_add_test(tg_test_property test_property.cpp)

