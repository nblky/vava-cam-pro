#include "platform.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static const char *helper_path;
static pid_t child;
static uint64_t deadline, until;
static unsigned duration, close_attempts;
static bool opening, close_requested, timed_out;
static const char *state="external_ap_management";
bool platform_managed(void) { return helper_path!=NULL; }
bool platform_init(const char *helper) {
    if (!helper) return true;
    struct stat st;
    if (*helper!='/' || lstat(helper,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() ||
        (st.st_mode&0022) || access(helper,X_OK)) return false;
    helper_path=helper; state="closed"; return true;
}
static bool launch(bool open_ap, const char *ssid, const char *password, uint64_t now) {
    int pipefd[2]; if (pipe(pipefd)) return false;
    (void)fcntl(pipefd[0],F_SETFD,FD_CLOEXEC); (void)fcntl(pipefd[1],F_SETFD,FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) { close(pipefd[0]); close(pipefd[1]); return false; }
    int error=posix_spawn_file_actions_adddup2(&actions,pipefd[0],STDIN_FILENO);
    if (!error) error=posix_spawn_file_actions_addclose(&actions,pipefd[0]);
    if (!error) error=posix_spawn_file_actions_addclose(&actions,pipefd[1]);
    if (!error) error=posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0);
    char *argv[]={(char *)helper_path,open_ap?"open":"close",NULL};
    posix_spawnattr_t attributes;
    int attr_error=posix_spawnattr_init(&attributes);
    if (!error) error=attr_error;
    if (!error) error=posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETPGROUP);
    if (!error) error=posix_spawnattr_setpgroup(&attributes,0);
    if (!error) error=posix_spawn(&child,helper_path,&actions,&attributes,argv,environ);
    if (!attr_error) posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions); close(pipefd[0]);
    if (error) { close(pipefd[1]); child=0; return false; }
    if (open_ap) {
        char input[96]; int n=snprintf(input,sizeof(input),"%s\n%s\n",ssid,password);
        size_t off=0;
        while (n>0 && off<(size_t)n) {
            ssize_t k=write(pipefd[1],input+off,(size_t)n-off);
            if (k<0 && errno==EINTR) continue;
            if (k<=0) break;
            off+=(size_t)k;
        }
        memset(input,0,sizeof(input));
        if (n<=0 || off!=(size_t)n) (void)kill(-child,SIGTERM);
    }
    close(pipefd[1]); opening=open_ap; deadline=now+30000; timed_out=false;
    state=open_ap?"opening":"closing"; return true;
}
bool platform_pair_open(const char *ssid, const char *password, unsigned seconds, uint64_t now) {
    if (!helper_path || child || close_requested || until || !seconds || seconds>300) return false;
    duration=seconds; close_attempts=0;
    return launch(true,ssid,password,now);
}
void platform_pair_close(uint64_t now) {
    if (!helper_path) return;
    until=0; close_requested=true; close_attempts=0;
    platform_tick(now);
}
static uint64_t close_retry_delay(void){
    return close_attempts>=6?30000:UINT64_C(1000)<<(close_attempts?close_attempts-1:0);
}
void platform_tick(uint64_t now) {
    if (!helper_path) return;
    if (child) {
        int result=0; pid_t done=waitpid(child,&result,WNOHANG);
        if (done==child || (done<0 && errno==ECHILD)) {
            child=0;
            bool ok=done>0 && WIFEXITED(result) && WEXITSTATUS(result)==0 && !timed_out;
            if (opening && ok && !close_requested) { until=now+duration*1000; state="open"; }
            else if (opening) { until=0; close_requested=true; state="open_failed"; }
            else if (ok) { close_requested=false; close_attempts=0; state="closed"; }
            else { state="close_failed"; deadline=now+close_retry_delay(); }
        } else if (now>=deadline) {
            (void)kill(-child,timed_out?SIGKILL:SIGTERM); timed_out=true; deadline=now+2000;
            until=0; close_requested=true; state="helper_timeout";
        }
    }
    if (until && now>=until) { until=0; close_requested=true; close_attempts=0; }
    /* Keep closing a possibly exposed temporary AP until the helper confirms
     * success. Back off after failures without forgetting the close request. */
    if (!child && close_requested &&
        (strcmp(state,"close_failed") || now>=deadline)) {
        if(close_attempts<6)++close_attempts;
        if (!launch(false,"","",now)) { state="close_failed"; deadline=now+close_retry_delay(); }
    }
}
uint64_t platform_pair_until(void) { return until; }
const char *platform_state(void) { return state; }
void platform_shutdown(uint64_t now) {
    if (!helper_path) return;
    platform_pair_close(now);
    /* Bounded shutdown; OpenWrt stop_service also closes the pairing AP. */
    for (unsigned n=0;n<100 && (child || close_requested);++n) {
        struct timespec delay={.tv_nsec=20000000}; nanosleep(&delay,NULL); platform_tick(now+20*(n+1));
    }
    if (child) { (void)kill(-child,SIGKILL); (void)waitpid(child,NULL,0); child=0; }
}
