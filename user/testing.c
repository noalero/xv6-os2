#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"


void part2(int loop_size){
    int pid = loop_size + 2;
    if(pid > 0){
        pid++;
    }
    //printf("  %d  \n", 22);
    // for (int i = 0; i < loop_size; i++){
    //     pid = fork();
    //     printf("  %d  \n", pid);
    //     if (pid == 0){ // Child
    //         printf("CHild's pid: %d\n", pid);
    //     }
    //     else { // Parent
    //         printf("Parent's pid: %d\n", pid);
    //     }
    // }
}

int main(int argc, char* argv[]){
    int loop_size = 2;
    part2(loop_size);
    return 0;
}