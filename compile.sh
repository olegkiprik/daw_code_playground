clang -Wall -Wextra -Wpedantic -Wshadow -Wstrict-aliasing -Werror=vla -Wno-unused-function -Wno-shift-op-parentheses -std=gnu17 -Os -ffast-math -march=x86-64-v2 -DCACHE_LINE=64 -DMA_NO_AVX2 -fstrict-aliasing main.c -lm

