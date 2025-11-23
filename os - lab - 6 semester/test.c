/*
 * a1.1-C.c
 *
 * A simple program in C counting the occurence of a character in a file
 * and writing the result in another file
 * 
 * Input is given from the command line without further tests:
 * argv[1]: file to read from
 * argv[2]: file to write to
 * argv[3]: character to search for
 *
 * Operating Systems course, CSLab, ECE, NTUA
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
 
 int main(int argc, char *argv[]) {
 
    
     char buff [1024]; 

     char cc, c2c; 
     int count = 0;
 
     /* open file for reading */
    int fd; 
    fd = open(argv[1], O_RDONLY); 
    if (fd== -1){
        perror("open"); 
        exit(1); 
    }
 
     /* open file for writing the result */
    int fd2; 
    fd2 = open(argv[2], O_WRONLY  |O_CREAT| O_TRUNC, 0644);
    if (fd2 == -1){
        perror("open-write");
        exit(1); 
    }
     /* character to search for (third parameter in command line) */
     c2c = argv[3][0];
 
     /* count the occurences of the given character */
     // impleemnting fgetc with the syst call read -reads file char by char-
     //reads one byte - 1 char -from fpr and saves it at buff 
     // if while ()=0 tehen EOF, =-1 then error
     while (read(fd, &buff, 1)){
     if (buff[0]== c2c) // if the char mateches teh  temporary saved char  count ++
      count++;
     }
  
     /* write the result in the output file */
     
     printf("found %d %c", count, c2c); 
     char count_str[20];
     int n = snprintf(count_str, sizeof(count_str), "%d", count);
     ssize_t bytes_written = write(fd2, count_str, n );
 
     close(fd); 
     close(fd2); 
     return 0;
 }
 