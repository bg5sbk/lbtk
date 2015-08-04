#include "./utils.h"

uint32_t main_flags = 0;
volatile int running = 1;

#define _freopen(to,mode,what) do { \
	what = freopen(to, mode, what); \
	assert(what != NULL); \
} while (0)

char** main_init (int argc, char **argv) {
	void sighandler_noop (int s) { (void) s; }
	void sighandler_stop (int s) { (void) s; running = 0; }
	signal(SIGPIPE, sighandler_noop);
	signal(SIGUSR1, sighandler_noop);
	signal(SIGUSR2, sighandler_noop);
	signal(SIGSTOP, sighandler_stop);
	signal(SIGINT,  sighandler_stop);
	signal(SIGTERM, sighandler_stop);
	_freopen ("/dev/null", "r", stdin);
	main_flags = 0;

    int first_positional = 1;
    for (int i=1; i<argc ;++i) {
        if (!strcmp(argv[i],"-f"))
            main_flags |= MF_FORK;
        else if (!strcmp(argv[i],"-d"))
            main_flags |= MF_DAEMONIZE_WANTED;
        else {
            first_positional = i;
            break;
        }
    }

    if (main_flags & MF_DAEMONIZE_WANTED) {
        char *cmd = strrchr(argv[0], '/');
        if (!cmd)
            cmd = argv[0];
        else ++cmd;
        openlog(cmd, LOG_NDELAY|LOG_PID, LOG_LOCAL0);
    }

    return argv+first_positional;
}

void main_run (void (*run)()) {
	int children[MAXCHLD], count_children=0, kill_sent=0;

	_freopen ("/dev/null", "a", stdout);
	if (main_flags & MF_DAEMONIZE_WANTED) {
		if (0 > daemon(1,0)) {
			LOG("daemon() error : (%d) %s", errno, strerror(errno));
			exit(2);
		} else {
            main_flags |= MF_DAEMONIZED;
			_freopen ("/dev/null", "a", stderr);
		}
	}

	if (!(main_flags & MF_FORK))
		return (*run)();
	else {
		for (int i=0; i<MAXCHLD ;++i) {
			children[i] = fork();
			if (children[i] < 0) {
				LOG("fork() failed : (%d) %s", errno, strerror(errno));
			} else if (children[i] == 0) { // break;
				memset(children, 0, sizeof(children));
				return (*run)();
			} else {
				++ count_children;
			}
		}

		while (count_children > 0) {

			if (!running && !kill_sent) {
				kill_sent = 1;
				for (int i=0; i<0 ;++i) {
					if (children[i] != 0)
						kill(children[i], SIGTERM);
				}
			}

			int pid, prc = 0;
			pid = waitpid(0, &prc, 0);
			if (pid < 0)
				break;
			LOG("Child exited [%d] with RC [%d]", pid, prc);
			-- count_children;
			for (int i=0; i<MAXCHLD ;++i) {
				if (children[i] && pid == children[i]) {
					children[i] = 0;
				}
			}
		}
	}
}

int sockaddr_init(struct sockaddr *sa, char *url)
{
	int rc;
	char *colon;
	memset(sa, 0, sizeof(struct sockaddr_in6));
	if (!(colon = strrchr(url, ':')))
		return 0;
	*colon = '\0';
	if (*(colon-1) == ']') {
		*(colon-1) = 0;
		S6FAM(sa) = AF_INET6;
		S6PRT(sa) = htons(atoi(colon+1));
		rc = inet_pton(AF_INET6, url+1, S6BUF(sa));
		*(colon-1) = ']';
	} else {
		S4FAM(sa) = AF_INET;
		S4PRT(sa) = htons(atoi(colon+1));
		rc = inet_pton(AF_INET, url, S4BUF(sa));
	}
	*colon = ':';
	return (rc == 1);
}

void sockaddr_dump(const struct sockaddr *sa, char *dst, size_t dlen)
{
    if (!inet_ntop(SAFAM(sa), SABUF(sa), dst, dlen))
        strncpy(dst, "?.?.?.?,?", dlen);
    else {
        size_t len = strlen(dst);
        snprintf(dst+len, dlen-len, ":%d", ntohs(SAPRT(sa)));
    }
}

void main_log(char *fmt, ...) {
    va_list arg;
    va_start(arg,fmt);
    if (main_flags & MF_DAEMONIZED) {
        vsyslog(LOG_INFO, fmt, arg);
    } else {
        vfprintf(stderr, fmt, arg);
    }
    va_end(arg);
}

