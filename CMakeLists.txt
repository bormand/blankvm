cmake_minimum_required(VERSION 3.10)

project(blankvm)

add_compile_options(-std=gnu99 -Wall -Wextra -Werror)

add_executable(
    blankvm
    src/blankvm.c
)


enable_testing()

function(add_test_on_asm name)
    set(asm ${CMAKE_CURRENT_SOURCE_DIR}/test/${name}.asm)
    set(bin ${name}.bin)
    set(in ${CMAKE_CURRENT_SOURCE_DIR}/test/in.txt)
    set(out ${CMAKE_CURRENT_SOURCE_DIR}/test/out.txt)

    add_custom_command(
        OUTPUT ${bin}
        COMMAND nasm -f bin -o ${bin} ${asm}
        DEPENDS ${asm}
    )

    add_custom_target(
        test_binary_${name} ALL
        DEPENDS ${bin}
    )

    add_test(
        NAME ${name}
        COMMAND sh -c "cat ${in} | ./blankvm ${ARGN} ${bin} | diff - ${out}"
    )
    set_tests_properties(${name} PROPERTIES TIMEOUT 5)
endfunction()

add_test_on_asm(test16)
add_test_on_asm(test32 -P)
