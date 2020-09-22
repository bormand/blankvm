cmake_minimum_required(VERSION 3.10)

project(blankvm)

add_compile_options(-std=gnu99 -Wall -Wextra -Werror)

add_executable(
    blankvm
    src/blankvm.c
)
