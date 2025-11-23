#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

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
       /*child process*/
       printf("Child:Hello from the child.My PID is %d and my parent's PID is %d.\n",getpid(),getppid());
       printf("Variable x in the child:%d\n");
       char *args[]={"./file1",argv[1],argv[2],argv[3],NULL};
       execv("./file1",args);
       printf("execv failed\n");

       exit(0);
    } 
    else {  
          /*parent process*/
          int status; 
          printf("Parent:My child's PID is %d.\n",pid);
          printf("Waiting child to terminate...\n");
          pid=wait(&status);
          explain_wait_status(pid,status);
          printf("Variable x in the parent:%d\n");
    }



    close(input_fd);
    close(output_fd);
    return 0;
}