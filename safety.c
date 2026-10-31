/* Run with an argument to try a mistake; Burrow should stop the program
   with a clear message instead of corrupting memory.
     ./safety double    -> double free
     ./safety bogus     -> freeing a pointer balloc never gave out */
#include "burrow.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: safety double|bogus\n"); return 0; }
    if (strcmp(argv[1], "double") == 0) {
        void *p = balloc(32);
        bfree(p);
        bfree(p);
    } else if (strcmp(argv[1], "bogus") == 0) {
        static char not_ours[64];
        bfree(not_ours + 16);
    }
    printf("not caught\n");
    return 0;
}
