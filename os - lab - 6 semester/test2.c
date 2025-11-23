#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    pid_t pid;

    // Δημιουργία διεργασίας παιδί
    pid = fork();

    if (pid < 0) {
        perror("fork failed");
        exit(1);
    } else if (pid == 0) {
        // Κώδικας που εκτελείται από το παιδί
        printf("Hello from child! My PID: %d, My Parent's PID: %d\n", getpid(), getppid());
        exit(0);
    } else {
        // Κώδικας που εκτελείται από τον γονέα
        printf("Hello from parent! My PID: %d, My Child's PID: %d\n", getpid(), pid);
        wait(NULL); // Ο γονέας περιμένει το παιδί να τελειώσει
        printf("Child process terminated.\n");
    }

    return 0;
}
