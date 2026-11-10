#ifndef VAVA_PROCESS_SIGNAL_H
#define VAVA_PROCESS_SIGNAL_H
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>

/* signal() can install a one-shot handler on glibc with strict POSIX feature
 * macros. Explicit sigaction flags keep reload and shutdown handlers installed. */
static inline bool process_signal_install(int number,void (*handler)(int)) {
    struct sigaction action={.sa_handler=handler,.sa_flags=SA_RESTART};
    return sigemptyset(&action.sa_mask)==0 && sigaction(number,&action,NULL)==0;
}
#endif
