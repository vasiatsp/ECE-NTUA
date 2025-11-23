#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

int main(int argc, char *argv[]) {
    const char *input_file = argv[1];
    const char *output_file = argv[2];
    char c2c = argv[3][0];
    int count = 0; 

    // open input file
    int input_fd = open(input_file, O_RDONLY);
    if (input_fd == -1) {
        perror("open");
        return 1;
    }
    // open output file
    int output_fd = open(output_file, O_WRONLY);
    if (output_fd == -1) {
        perror("open");
        close(input_fd);
        return 1;
    }

    int pid = fork();

    if (pid == -1) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {  
        // child process - count the num of the given char
        int count_curr = 0;
        char cc;
        ssize_t bytes_read;

        while ((bytes_read = read(input_fd, &cc, sizeof(cc))) > 0) {
         for(ssize_t i=0; i<bytes_read; i++ ) {
              if (cc == c2c) {
                count_curr++;
            }
        }
        }
        printf("Child %d counted: %d\n", getpid(), count_curr);
        exit(count_curr);
    } else {  // Parent process
      int answer, status;
            while ((answer = wait(&status)) > 0) {
                if (WIFEXITED(status)) {
                    count += WEXITSTATUS(status);
        }
    }
    }

    // Write the result to the output file
char count_str[20];
int n = snprintf(count_str, sizeof(count_str), "%d", count);
ssize_t bytes_written = write(output_fd, count_str, n);
if (bytes_written == -1) {
 perror("write");
 close(input_fd);
 close(output_fd);
 return 1;
}

    close(input_fd);
    close(output_fd);
    return 0;
}

