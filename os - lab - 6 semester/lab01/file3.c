#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

#define P 6
int active_children = 0;

void sigint_handler(int sig) {
    printf("The active children searchers are %d\n", active_children);
}

void explain_wait_status(pid_t pid, int status) {
    if (WIFEXITED(status)) {
        fprintf(stderr, "Child with PID=%ld terminated normally, exit status=%d\n", (long)pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "Child with PID=%ld was terminated by a signal, signo=%d\n", (long)pid, WTERMSIG(status));
    } else if (WIFSTOPPED(status)) {
        fprintf(stderr, "Child with PID=%ld has been stopped by a signal, signo=%d\n", (long)pid, WSTOPSIG(status));
    } else {
        fprintf(stderr, "%s: Internal error: Unhandled case, PID=%ld, status=%d\n", __func__, (long)pid, status);
        exit(1);
    }
    fflush(stderr);
}

int main(int argc, char *argv[]) {
    int fd1, fd2;
    char cc, c2c;
    int count = 0;

    if ((fd1 = open(argv[1], O_RDONLY)) == -1) {
        perror("Problem opening file to read");
        return -1;
    }

    if ((fd2 = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644)) == -1) {
        perror("Problem opening file to write");
        return -1;
    }

    c2c = argv[3][0];

    int i, status, num, sum = 0;
    pid_t p;
    int fd[2];

    if (pipe(fd) < 0) {
        perror("pipe");
        exit(1);
    }

    off_t total_size, start_off, end_off, remaining_bytes, partial_size;
    total_size = lseek(fd1, 0, SEEK_END);
    printf("The total size of the input file is %ld bytes\n", total_size);
    start_off = 0;
    remaining_bytes = total_size % P;
    partial_size = total_size / P;
    end_off = start_off + partial_size;
    lseek(fd1, 0, SEEK_SET);

    for (i = 0; i < P; i++) {
        p = fork();

        if (p < 0) {
            perror("fork");
            exit(1);
        } else if (p == 0) {
            close(fd[0]);
            count = 0;

            if (i == P - 1) end_off += remaining_bytes;
            printf("The child process %d with PID %d will read %ld bytes\n", i + 1, getpid(), end_off - start_off);
            lseek(fd1, start_off, SEEK_SET);
            while (start_off < end_off && read(fd1, &cc, 1) > 0) {
                if (c2c == cc) count++;
                start_off++;
            }

            if (write(fd[1], &count, sizeof(count)) != sizeof(count)) {
                perror("write to pipe");
                exit(1);
            }

            close(fd[1]);
            exit(0);
        } else {
            active_children++;
        }
        start_off = end_off;
        end_off += partial_size;
    }

    close(fd[1]);
    signal(SIGINT, sigint_handler);

    for (int j = 0; j < P; j++) {
        p = wait(&status);
        sleep(2);
        if (read(fd[0], &num, sizeof(num)) != sizeof(num)) {
            perror("read from pipe");
            exit(1);
        }

        active_children--;
        explain_wait_status(p, status);
        sum += num;
    }

    close(fd[0]);

    dprintf(fd2, "The character '%c' appears %d times in the input file named %s\n", c2c, sum, argv[1]);

    close(fd1);
    close(fd2);

    return 0;
}
