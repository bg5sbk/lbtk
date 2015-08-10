#ifndef LB_UTILS_H
#define LB_UTILS_H 1

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdarg.h>
#include <syslog.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#define MF_DAEMONIZE_WANTED 0x01
#define MF_FORK             0x02
#define MF_DAEMONIZED       0x10

#define MAXEVT  64
#define MAXCHLD 2
#define PIPE_SIZE 524288

#ifdef HAVE_ASSERT
#define ASSERT(x) assert((x))
#else
#define ASSERT(...)
#endif

#define LOG(FMT,...) main_log(FMT"\n", ##__VA_ARGS__)

#ifdef HAVE_DEBUG
#define DEBUG(FMT,...) LOG(FMT,##__VA_ARGS__)
#else
#define DEBUG(...)
#endif

#define ACCESS(FMT,...) LOG(FMT,##__VA_ARGS__)

//------------------------------------------------------------------------------

#define S4(p)    ((struct sockaddr_in*)(p))
#define S4LEN(p) sizeof(struct sockaddr_in)
#define S4BUF(p) (void*)&(S4(p)->sin_addr)
#define S4FAM(p) S4(p)->sin_family
#define S4PRT(p) S4(p)->sin_port

#define S6(p)    ((struct sockaddr_in6*)(p))
#define S6LEN(p) sizeof(struct sockaddr_in6)
#define S6BUF(p) (void*)&(S6(p)->sin6_addr)
#define S6FAM(p) S6(p)->sin6_family
#define S6PRT(p) S6(p)->sin6_port

#define SA(p)    ((struct sockaddr*)(p))
#define SAFAM(p) SA(p)->sa_family
#define SABUF(p) ((SAFAM(p)==AF_INET) ? S4BUF(p) : S6BUF(p))
#define SAPRT(p) ((SAFAM(p)==AF_INET) ? S4PRT(p) : S6PRT(p))
#define SALEN(p) ((SAFAM(p)==AF_INET) ? S4LEN(p) : S6LEN(p))

//------------------------------------------------------------------------------

#define IDLE_STRUCT_NAME(T) IDLE_##T
#define DIRTY_STRUCT_NAME(T) DIRTY_##T
#define ACTIVE_STRUCT_NAME(T) ACTIVE_##T

#define ACQUIRE_STRUCT_CALL(T) acquire_##T()
#define ACQUIRE_STRUCT_DECL(T) static T * acquire_##T () { \
	T *_p; \
	if (!(_p = IDLE_STRUCT_NAME(T))) { \
		if (0 != posix_memalign((void**)&_p, sizeof(long long), sizeof(T))) \
			abort(); \
		memset(_p, 0, sizeof(T)); \
	} else { \
		IDLE_STRUCT_NAME(T) = _p->next; \
		_p->next = NULL; \
	} return _p; \
}

#define SHIFT_STRUCT(b,p) do { \
	(p) = (b); \
	(b) = (b)->next; \
	(p)->next = NULL; \
} while (0)

#define PREPEND_STRUCT(b,p) do { \
	(p)->next = (b); \
	(b) = (p); \
} while (0)

#define MOVE_STRUCT(T,b0,b1) do { \
	T *_p; SHIFT_STRUCT(b0,_p); \
	PREPEND_STRUCT(b1,_p); \
} while (0)

#define DRAIN_STRUCT_CALL(T) recover_##T ()
#define DRAIN_STRUCT_DECL(T) static inline void recover_##T () { \
	while (DIRTY_STRUCT_NAME(T) != NULL) \
		MOVE_STRUCT(T,DIRTY_STRUCT_NAME(T), IDLE_STRUCT_NAME(T)); \
}

#define PURGE_STRUCT_CALL(T) purge_##T()
#define PURGE_STRUCT_DECL(T) static inline void purge_##T () { \
	while (IDLE_STRUCT_NAME(T) != NULL) { \
		T *_p; SHIFT_STRUCT(IDLE_STRUCT_NAME(T),_p); \
		free(_p); \
	} \
}

//------------------------------------------------------------------------------

extern uint32_t main_flags;
extern volatile int running;

int sockaddr_init (struct sockaddr *sa, char *url);
void sockaddr_dump (const struct sockaddr *sa, char *dst, size_t dlen);
void sock_set_chatty (int fd, int on);

char **main_init (int argc, char **argv);
void main_run (void (*run) ());
void main_log (char *fmt, ...);

#endif
