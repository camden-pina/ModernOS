#include <pthread.h>
#include <stdio.h>

static _Thread_local volatile unsigned long value;

static void *worker(void *argument) {
    (void)argument;
    if(value)return (void *)1;
    value=0x434f415354UL;
    return NULL;
}

int main(void) {
    pthread_t thread;
    void *result;
    if(value)return 1;
    value=123;
    if(pthread_create(&thread,NULL,worker,NULL) || pthread_join(thread,&result))return 2;
    if(result || value!=123)return 3;
    puts("TLS_ZERO_PASS");
    return 0;
}
