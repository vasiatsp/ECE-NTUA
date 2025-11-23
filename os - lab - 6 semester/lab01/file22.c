#include <unistd.h>
#include <stdio.h>

int main() {
    int x = 5;
    int pid = fork();

    if (pid == -1) {
        perror("fork");
    }

    if (pid != 0) {
        printf("The value of x in the parent process is: %d.\n", x);
        wait();
        printf("The value of x in the parent process after the call of the child process is: %d.\n", x);
    } else {
        x = 8;
        printf("The value of x in the child process is: %d.\n", x);
    }

    return 0;
}
