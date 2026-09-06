add_executable(tailgov_d tailgov_d.cpp)
target_link_libraries(tailgov_d PRIVATE TailGovernor::tailgovernor ws2_32)
if(MSVC)
    target_compile_options(tailgov_d PRIVATE /W4 /WX)
else()
    target_compile_options(tailgov_d PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME tg_distributed
    COMMAND tailgov_d coordinator ${CMAKE_CURRENT_BINARY_DIR}/dist_state.bin ${CMAKE_CURRENT_BINARY_DIR}/dist_result.txt)

