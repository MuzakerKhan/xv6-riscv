// System call numbers
#define SYS_fork    1
#define SYS_exit    2
#define SYS_wait    3
#define SYS_pipe    4
#define SYS_read    5
#define SYS_kill    6
#define SYS_exec    7
#define SYS_fstat   8
#define SYS_chdir   9
#define SYS_dup    10
#define SYS_getpid 11
#define SYS_sbrk   12
#define SYS_pause  13
#define SYS_uptime 14
#define SYS_open   15
#define SYS_write  16
#define SYS_mknod  17
#define SYS_unlink 18
#define SYS_link   19
#define SYS_mkdir  20
#define SYS_close           21
// system calls added for the deadlock detection project
#define SYS_dlstate         22  // print deadlock system state
#define SYS_dlacquire       23  // grab a user token, blocks if taken
#define SYS_dlrelease       24  // release a user token
#define SYS_dlsetmode       25  // switch between normal and aggressive mode
#define SYS_dlsetresolution 26  // switch between kill and preempt mode
#define SYS_setpriority     27  // set this process kill priority (0 to 9)
