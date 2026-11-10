#include <stdio.h>
#include <unistd.h>
int main(void) {
    printf("Hello from a locally built Coast package (uid=%lu)!\n", (unsigned long)getuid());
    return 0;
}
