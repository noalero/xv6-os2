#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"



void
part2(int loop_size){
    int pid = loop_size + 2;
    if(pid > 0){
        pid++;
    }
}

void
linked_list (int loop_size){}

void
part3 (int loop_size){
    int pid = -3;
    for(int i = 0; i < loop_size; i++){
        pid = fork();

        if(pid < 0){
            printf("part3: fork faild\n");
            exit(1);
        }

        if(pid == 0 ){ // Child
            printf("Child's process\n");
            exit(0);
        }

        if(pid > 0){ // Parent
            printf("before wait\n");
            wait(0);
            printf("Parent's process pid = %d\n", pid);
        }
    }    
}
    

int
main(int argc, char* argv[]){
    int loop_size = 1;
    if(argc > 0) printf("%s:\n", argv[0]);
    printf("Number of arguments passed : %d\n", argc);
    if(argc == 2) {
        loop_size = atoi(argv[1]);
        printf("argv = %d\n", loop_size);
    }
    part3(loop_size);
    printf("After part3\n");
    return 1;
}