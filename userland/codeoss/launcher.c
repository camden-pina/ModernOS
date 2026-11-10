#define _POSIX_C_SOURCE 200809L
/* Original Coast supervisor. Large ELF startup AND wait/reaping happen in
 * this child, not in the compositor. The model owns the protocol pipe ends.
 * The desktop only waits for this small process after the model is reaped. */
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
static volatile sig_atomic_t model_pid;
static void terminate_model(int signal_number) {
    (void)signal_number;
    if (model_pid > 0) kill((pid_t)model_pid, SIGKILL);
}
int main(void) {
    struct sigaction action = {0};
    action.sa_handler = terminate_model;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL)) { perror("codeoss-launch: sigaction"); return 127; }
    pid_t child = fork();
    if (child < 0) { perror("codeoss-launch: fork"); return 127; }
    if (!child) {
        signal(SIGTERM, SIG_DFL);
        char *args[] = {"/bin/codeoss-model", NULL};
        execv(args[0], args);
        perror("codeoss-launch: exec");
        _exit(127);
    }
    model_pid = child;
    close(0);
    close(1);
    fprintf(stderr, "CODE_MODEL_CHILD launcher=%ld worker=%ld\n", (long)getpid(), (long)child);
    int status = 0;
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    model_pid = 0;
    if (result < 0) { perror("codeoss-launch: waitpid"); return 127; }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
