#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    int pid = fork();

    if (pid == -1) {
        perror("fork");
        exit(1);
    }

    if (pid != 0) {
        printf("The PID of my child is %d\n", pid);
        wait(NULL);
    } else {
        printf("Hello, the child's PID is %d and the parent's PID is %d\n", getpid(), getppid());
    }

    return 0;
}
