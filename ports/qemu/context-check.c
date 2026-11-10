#define _XOPEN_SOURCE 700
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>

static ucontext_t main_context, child_context;
static unsigned char stack[65536];
static volatile int phase;

static void require(int condition) {
    if (!condition) {
        fputs("COAST_CONTEXT_FAIL\n", stderr);
        exit(1);
    }
}

static void child(void) {
    sigset_t mask;
    require(sigprocmask(SIG_SETMASK, NULL, &mask) == 0);
    require(sigismember(&mask, SIGUSR1) == 1);
    require(sigismember(&mask, SIGUSR2) == 0);
    phase = 1;
    require(swapcontext(&child_context, &main_context) == 0);
    require(phase == 2);
    require(sigprocmask(SIG_SETMASK, NULL, &mask) == 0);
    require(sigismember(&mask, SIGUSR1) == 1);
    phase = 3;
}

int main(void) {
    sigset_t original, mask, observed;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    require(sigprocmask(SIG_SETMASK, &mask, &original) == 0);
    require(getcontext(&child_context) == 0);
    child_context.uc_stack.ss_sp = stack;
    child_context.uc_stack.ss_size = sizeof stack;
    child_context.uc_link = &main_context;
    sigemptyset(&child_context.uc_sigmask);
    sigaddset(&child_context.uc_sigmask, SIGUSR1);
    makecontext(&child_context, child, 0);
    require(swapcontext(&main_context, &child_context) == 0);
    require(phase == 1);
    require(sigprocmask(SIG_SETMASK, NULL, &observed) == 0);
    require(sigismember(&observed, SIGUSR1) == 0);
    require(sigismember(&observed, SIGUSR2) == 1);
    phase = 2;
    require(swapcontext(&main_context, &child_context) == 0);
    require(phase == 3);
    require(sigprocmask(SIG_SETMASK, &original, NULL) == 0);
    puts("COAST_CONTEXT_PASS stack, resume, return link, signal masks");
    return 0;
}
