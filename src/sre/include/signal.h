#ifndef SRE_SIGNAL_H
#define SRE_SIGNAL_H

#define SIGPIPE 13
#define SIG_IGN ((void (*)(int))1)

typedef void (*sighandler_t)(int);
sighandler_t signal(int signum, sighandler_t handler);

#endif
