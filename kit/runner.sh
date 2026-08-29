#!/bin/bash
gcc -O2 -g -std=c11 -Wall -Wextra -pthread -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -c src/update.c -o /dev/null 2>&1
echo $?
