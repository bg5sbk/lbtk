#include <fcntl.h>

#include <sys/resource.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <nanomsg/nn.h>
#include <nanomsg/pipeline.h>

#include "./utils.h"

#define FLAG_SHUT_SENT    0x0001
#define FLAG_SHUT_RECV    0x0002
#define FLAG_SHUT_BOTH    (FLAG_SHUT_SENT|FLAG_SHUT_RECV)

#define FLAG_MONITORED    0x0004
#define FLAG_ACTIVE       0x0008
#define FLAG_LISTED       (FLAG_MONITORED|FLAG_ACTIVE)

#define FLAG_ERRONEOUS    0x0020
#define FLAG_ACTIVITY     (FLAG_ERRONEOUS)

#define FLAG_REGISTERED   0x0040

#define SETONE(F,S,O) (((F)&(~(S)))|(O))
#define SETLIST(F,L)  SETONE(F,FLAG_LISTED,L)
#define SETMON(F)     SETLIST(F,FLAG_MONITORED)
#define SETACT(F)     SETLIST(F,FLAG_ACTIVE)

#define ISANY(F,O)    ((F)&(O))
#define ISALL(F,O)    (ISANY(F,O)==(O))

#define ISERR(i)      ISANY((i)->flags,FLAG_ERRONEOUS)
#define ISSHUT(i)     ISALL((i)->flags,FLAG_SHUT_BOTH)
#define ISLISTED(i)     ISANY((i)->flags,FLAG_LISTED)
#define ISACTIVE(i)     ISALL((i)->flags,FLAG_ACTIVE)
#define ISMONITORED(i)  ISALL((i)->flags,FLAG_MONITORED)
#define ISREGISTERED(i) ISALL((i)->flags,FLAG_REGISTERED)

#define BOTH (EPOLLIN|EPOLLOUT)

typedef struct monitored_s monitored_t;
typedef struct proxy_s proxy_t;
typedef struct pipe_s pipe_t;
typedef struct tunnel_s tunnel_t;
typedef struct channel_s channel_t;

enum item_type_e
{ PROXY = 1, CHANNEL };

#define MONITORED_FIELDS \
    void *next; \
    uint32_t flags; \
    uint32_t events; \
    enum item_type_e type

struct monitored_s
{
    MONITORED_FIELDS;
};

struct proxy_s
{
    MONITORED_FIELDS;
    struct
    {
        unsigned int count;
        unsigned int max;
    } pipes;
    int sock_front;
    int nn_feed;
};

struct channel_s
{
    MONITORED_FIELDS;
    enum
    { CONNECTING = 1, CONNECTED } status;
    tunnel_t *tunnel;
    channel_t *peer;
    pipe_t *tosend;
    int sock;
    const char *which;
};

struct tunnel_s
{
    uint64_t id;
    proxy_t *proxy;
    tunnel_t *next;             // IDLE, DIRTY, NULL
    channel_t front, back;
};

struct pipe_s
{
    pipe_t *next;               // IDLE, NULL
    int load;
    int fd[2];
};

// Tunnels are indirectly referenced in epoll_event structures via
// the channel_t poiters. One tunnel is referenced 2 times, so
// we cannot clean a tunnel based on a channel's event.
// The idea is then to play with 2 cache lists : one for really
// idle structures (not being monitored with epoll) and one for
// tunnels that vahe been clean during an epoll_ctl round.
static tunnel_t *IDLE_STRUCT_NAME (tunnel_t) = NULL;
static tunnel_t *DIRTY_STRUCT_NAME (tunnel_t) = NULL;

// Pipes do not have this problem, because hey are only pointed
// once, in the channel_t structures.
static pipe_t *IDLE_STRUCT_NAME (pipe_t) = NULL;

static channel_t *ACTIVE_STRUCT_NAME (channel_t) = NULL;

static proxy_t *ACTIVE_STRUCT_NAME (proxy_t) = NULL;

static int fd_epoll = -1;
static int count_epoll = 0;
static int front_backlog = 8192;

static int opt_buffer_size = 1;
static int opt_chatty_update = 1;
static int opt_chatty_front = 1;
static int opt_chatty_back = 1;

static uint64_t next_tunnel_id = 0;

/* -------------------------------------------------------------------------- */

ACQUIRE_STRUCT_DECL (pipe_t);
PURGE_STRUCT_DECL (pipe_t);

static void channel_close (channel_t * chan);
static void channel_shut (channel_t * chan);
static void channel_transfer (channel_t * src);
static void channel_manage_events (channel_t * src, uint32_t events);

ACQUIRE_STRUCT_DECL (tunnel_t);
PURGE_STRUCT_DECL (tunnel_t);
DRAIN_STRUCT_DECL (tunnel_t);

static tunnel_t *tunnel_reserve (proxy_t * proxy);
static void tunnel_init (tunnel_t * t);
static void tunnel_release (tunnel_t * t);
static void tunnel_unref (tunnel_t * t);
static void tunnel_abort (tunnel_t * t, const char *fmt, ...);

static void proxy_register (proxy_t * p);
static void proxy_pause (proxy_t * p);
static void proxy_resume (proxy_t * p);

/* -------------------------------------------------------------------------- */

#ifdef HAVE_DEBUG
/* <d0> must be at least 5 characters long */
static const char *
_evt2str (uint32_t evt, char *d0)
{
    char *d = d0;
    void add (char c)
    {
        *(d++) = c;
        *d = '\0';
    }
    *d0 = '\0';
    if (evt & EPOLLIN)
        add ('I');
    if (evt & EPOLLOUT)
        add ('O');
    if (evt & EPOLLERR)
        add ('E');
    if (evt & EPOLLHUP)
        add ('H');
    return d0;
}

/* <d0> must be at least 7 characters long */
static const char *
_flags2str (uint32_t status, char *d0)
{
    char *d = d0;
    void add (char c)
    {
        *(d++) = c;
        *d = '\0';
    }
    *d0 = '\0';
    if (status & FLAG_SHUT_SENT)
        add ('>');
    if (status & FLAG_SHUT_RECV)
        add ('<');
    if (status & FLAG_MONITORED)
        add ('M');
    if (status & FLAG_ACTIVE)
        add ('A');
    if (status & FLAG_ERRONEOUS)
        add ('E');
    if (status & FLAG_REGISTERED)
        add ('R');
    return d0;
}
#endif

static void
pipe_release (pipe_t ** pp)
{
    pipe_t *p = *pp;

    if (!p)
        return;
    if (p->load > 0) {
        close (p->fd[0]);
        close (p->fd[1]);
        p->fd[0] = p->fd[1] = -1;
        p->load = 0;
    }
    PREPEND_STRUCT (IDLE_STRUCT_NAME (pipe_t), p);
    *pp = NULL;
}

static pipe_t *
pipe_init ()
{
    pipe_t *p = ACQUIRE_STRUCT_CALL (pipe_t);

    if (p->fd[0] <= 0 && p->fd[1] <= 0) {
        if (0 > pipe2 (p->fd, SOCK_NONBLOCK | SOCK_CLOEXEC)) {
            pipe_release (&p);
            return NULL;
        }
        fcntl (p->fd[1], F_SETPIPE_SZ, PIPE_SIZE);
    }
    return p;
}

// Return a boolean value, FALSE if an error occured, TRUE if no socket
// error was met.
static void
_pipe_resume (channel_t * chan)
{
    pipe_t *p = chan->tosend;

    chan->tosend = NULL;
    while (p->load) {
        int rc = splice (p->fd[0], 0, chan->sock, 0, p->load,
            SPLICE_F_MOVE | SPLICE_F_MORE | SPLICE_F_NONBLOCK);

        if (rc < 0) {
            chan->events &= ~EPOLLOUT;
            if (errno == EAGAIN)
                chan->tosend = p;
            else {
                chan->flags |= FLAG_ERRONEOUS;
                pipe_release (&p);
            }
            return;
        }
        p->load -= rc;
    }
    pipe_release (&p);
}

/* -------------------------------------------------------------------------- */

static void
channel_close (channel_t * chan)
{
    if (chan->sock < 0)
        return;
    if (ISMONITORED (chan))
        --count_epoll;
    close (chan->sock);
    chan->sock = -1;
    chan->flags = chan->events = 0;
    pipe_release (&chan->tosend);
}

static void
channel_shut (channel_t * chan)
{
    if (chan->flags & FLAG_SHUT_SENT)
        return;
    if (chan->tosend)
        return;
    chan->flags |= FLAG_SHUT_SENT;
    shutdown (chan->sock, SHUT_WR);
    chan->events &= ~EPOLLOUT;
    pipe_release (&chan->tosend);
}

static void
channel_transfer (channel_t * src)
{
    src->flags &= ~FLAG_ACTIVITY;

    pipe_t *p;

    if (!(p = src->peer->tosend))
        p = pipe_init ();
    src->peer->tosend = NULL;
    if (!p) {
        src->flags |= FLAG_ERRONEOUS;
        return;
    }

    int rc = splice (src->sock, 0, p->fd[1], 0, PIPE_SIZE,
        SPLICE_F_MOVE | SPLICE_F_MORE | SPLICE_F_NONBLOCK);

    if (rc == 0) {
        src->events &= ~EPOLLIN;
        src->flags |= FLAG_SHUT_RECV;
    }
    else if (rc < 0) {
        src->events &= ~EPOLLIN;
        if (errno != EAGAIN)
            src->flags |= FLAG_ERRONEOUS;
    }
    else {
        p->load += rc;
    }

    if (p->load <= 0)
        return pipe_release (&p);
    src->peer->tosend = p;
    return _pipe_resume (src->peer);
}

static void
channel_rearm (channel_t * chan, uint32_t io)
{
    struct epoll_event evt;
    int rc;

    evt.data.ptr = chan;
    evt.events = io | EPOLLET | EPOLLONESHOT;

    if (FLAG_SHUT_BOTH == (chan->flags & FLAG_SHUT_BOTH)) {
        if (ISMONITORED (chan))
            --count_epoll;
        if (ISREGISTERED (chan)) {
retry:
            rc = epoll_ctl (fd_epoll, EPOLL_CTL_DEL, chan->sock, NULL);
            if (rc < 0) {
                if (errno == EINTR)
                    goto retry;
                if (errno != ENOENT) {
                    ASSERT (rc == 0);
                }
            }
        }
        chan->flags &= ~(FLAG_LISTED | FLAG_ACTIVITY | FLAG_REGISTERED);
    }

    if (ISREGISTERED (chan)) {
        if (io != chan->events) {
            rc = epoll_ctl (fd_epoll, EPOLL_CTL_MOD, chan->sock, &evt);
            ASSERT (rc == 0);
        }
    }
    else {
        rc = epoll_ctl (fd_epoll, EPOLL_CTL_ADD, chan->sock, &evt);
        ASSERT (rc == 0);
    }

    (void) rc;
    if (!ISMONITORED (chan))
        ++count_epoll;
    chan->events = io;
    chan->flags = SETONE (chan->flags, FLAG_LISTED | FLAG_ACTIVITY,
        FLAG_MONITORED | FLAG_REGISTERED);
}

static void
channel_patch (channel_t * c)
{
    if (c->flags & FLAG_SHUT_RECV) {
        c->events &= ~EPOLLIN;
        channel_shut (c->peer);
        if (c->tosend)
            pipe_release (&c->tosend);
    }
}

static inline uint32_t
channel_events (channel_t * c)
{
    uint32_t evt = 0;

    if ((c->status == CONNECTING || c->tosend)
        && !(c->flags & FLAG_SHUT_SENT))
        evt |= EPOLLOUT;
    if (c->peer->status == CONNECTED && !(c->peer->flags & FLAG_SHUT_SENT)
        && !(c->flags & FLAG_SHUT_RECV)
        && c->peer->tosend == NULL)
        evt |= EPOLLIN;
    return evt;
}

static void
channel_update_listed (channel_t * c)
{
    char e[8], s[8];

    DEBUG ("%llu %s %x/%s/%s (%d) %s", c->tunnel->id, c->which, c->status,
        _flags2str (c->flags, s), _evt2str (c->events, e), count_epoll,
        __FUNCTION__);
    ASSERT (ISLISTED (c));
    uint32_t evt = channel_events (c);

    if (ISMONITORED (c)) {
        channel_rearm (c, evt);
    }
    else if (ISACTIVE (c)) {
        c->events = evt;
    }
}

static void
channel_update (channel_t * c)
{
    char e[8], s[8];

    DEBUG ("%llu %s %x/%s/%s (%d) %s", c->tunnel->id, c->which, c->status,
        _flags2str (c->flags, s), _evt2str (c->events, e), count_epoll,
        __FUNCTION__);
    ASSERT (!ISLISTED (c));

    channel_patch (c);
    channel_patch (c->peer);
    if (ISSHUT (c) && ISSHUT (c->peer))
        return tunnel_release (c->tunnel);
    if (ISERR (c))
        return tunnel_abort (c->tunnel, "Peer error: %s", c->which);
    if (ISERR (c->peer))
        return tunnel_abort (c->tunnel, "Peer error: %s", c->which);

    uint32_t evt = channel_events (c);

    if (c->events & BOTH) {
        c->events = evt;
        c->flags = SETACT (c->flags) & ~FLAG_ACTIVITY;
        PREPEND_STRUCT (ACTIVE_STRUCT_NAME (channel_t), c);
    }
    else {
        channel_rearm (c, evt);
    }

    DEBUG ("%llu %s %x/%s/%s DONE (%d) %s", c->tunnel->id, c->which, c->status,
        _flags2str (c->flags, s), _evt2str (c->events, e), count_epoll,
        __FUNCTION__);
    channel_update_listed (c->peer);
}

static void
channel_manage_events (channel_t * c, uint32_t events)
{
    char e[8], s[8];

    DEBUG ("%llu %s %x/%s/%s (%d) %s", c->tunnel->id, c->which, c->status,
        _flags2str (c->flags, s), _evt2str (events, e), count_epoll,
        __FUNCTION__);
    ASSERT (!(c->flags & FLAG_LISTED));

    if (events & EPOLLERR)
        return tunnel_abort (c->tunnel, "Channel error: %s", c->which);

    if (!c->status)             // Deleted!
        return;
    if (events & EPOLLOUT) {
        if (c->status == CONNECTING) {
            c->status = CONNECTED;
            return channel_update (c);
        }
    }
    if (c->tosend) {
        ASSERT (c->status == CONNECTED);
        _pipe_resume (c);
    }
    if (events & EPOLLIN)
        channel_transfer (c);
    if (events & EPOLLHUP)
        c->flags |= FLAG_SHUT_RECV;
    return channel_update (c);
}

/* -------------------------------------------------------------------------- */

static void
tunnel_init (tunnel_t * t)
{
    t->front.flags = t->back.flags = 0;
    t->front.events = t->back.events = 0;
    t->front.sock = t->back.sock = -1;
    t->front.tunnel = t->back.tunnel = t;
    t->front.type = t->back.type = CHANNEL;
    t->front.status = t->back.status = 0;
    t->front.tosend = t->back.tosend = NULL;
    t->front.next = t->back.next = NULL;
    t->front.peer = &t->back;
    t->back.peer = &t->front;
    t->back.which = "BACK";
    t->front.which = "FRONT";
}

static tunnel_t *
tunnel_reserve (proxy_t * proxy)
{
    tunnel_t *t = ACQUIRE_STRUCT_CALL (tunnel_t);

    t->proxy = proxy;
    tunnel_init (t);
    t->id = next_tunnel_id++;
    return t;
}

static void
tunnel_release (tunnel_t * t)
{
    channel_close (&t->front);
    channel_close (&t->back);
    tunnel_init (t);
    PREPEND_STRUCT (IDLE_STRUCT_NAME (tunnel_t), t);
}

static void
tunnel_unref (tunnel_t * t)
{
    proxy_t *p = t->proxy;

    tunnel_release (t);
    if (p->pipes.max == (p->pipes.count--))
        proxy_resume (p);
}

static void
tunnel_abort (tunnel_t * t, const char *fmt, ...)
{
    char buf[256];
    va_list arg;

    va_start (arg, fmt);
    vsnprintf (buf, sizeof (buf), fmt, arg);
    va_end (arg);

    LOG ("Tunnel aborted: %s", buf);
    tunnel_unref (t);
}

static void
tunnel_register (tunnel_t * t)
{
    t->front.status = CONNECTED;
    t->front.events = t->back.events = 0;
    t->back.status = CONNECTING;
    channel_rearm (&t->front, 0);
    channel_rearm (&t->back, EPOLLOUT);
}

/* -------------------------------------------------------------------------- */

static void
proxy_register (proxy_t * p)
{
    int op = ISREGISTERED (p) ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    struct epoll_event evt;

    evt.data.ptr = p;
    evt.events = EPOLLET | EPOLLONESHOT | EPOLLIN;
    int rc = epoll_ctl (fd_epoll, op, p->sock_front, &evt);

    ASSERT (rc == 0);
    (void) rc;
    if (!(p->flags & FLAG_MONITORED))
        ++count_epoll;
    p->flags |= FLAG_REGISTERED | FLAG_MONITORED;
}

static void
proxy_pause (proxy_t * p)
{
    p->events = 0;
    if (!ISMONITORED (p))
        return;
    struct epoll_event evt;

    evt.data.ptr = p;
    evt.events = EPOLLET | EPOLLONESHOT;

    int rc = epoll_ctl (fd_epoll, EPOLL_CTL_MOD, p->sock_front, &evt);

    ASSERT (rc == 0);
    (void) rc;
    --count_epoll;
    p->flags &= ~FLAG_LISTED;
}

static void
proxy_resume (proxy_t * p)
{
    ASSERT (!(p->flags & FLAG_LISTED));
    p->flags |= FLAG_ACTIVE;
    p->events = EPOLLIN;
    PREPEND_STRUCT (ACTIVE_STRUCT_NAME (proxy_t), p);
}

static void
proxy_init (proxy_t * p)
{
    p->next = NULL;
    p->flags = 0;
    p->events = 0;
    p->type = PROXY;
    p->sock_front = -1;
    p->nn_feed = -1;
    struct rlimit rl;

    if (0 != getrlimit (RLIMIT_NOFILE, &rl))
        abort ();
    rl.rlim_cur = rl.rlim_max;
    setrlimit (RLIMIT_NOFILE, &rl);
    p->pipes.count = 0;
    p->pipes.max = rl.rlim_max / 2;
    LOG ("p.max = %d", p->pipes.max);

}

static void
proxy_init_front (proxy_t * p, char *front)
{
    struct sockaddr_in6 ss;

    sockaddr_init (SA (&ss), front);
    p->sock_front = socket (SAFAM (&ss), SOCK_STREAM | O_NONBLOCK, 0);
    ASSERT (p->sock_front >= 0);

    sock_set_chatty (p->sock_front, 1);

    int opt = 1;

    setsockopt (p->sock_front, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof (opt));

    if (0 > bind (p->sock_front, SA (&ss), SALEN (&ss))) {
        LOG ("front(%d).bind(%s) failed", p->sock_front, front);
        exit (1);
    }

    LOG ("front(%s) ready", front);
}

static void
proxy_init_feeders (proxy_t * p, char **feeds)
{
    int opt;

    if (0 > (p->nn_feed = nn_socket (AF_SP, NN_PULL))) {
        LOG ("feeder.socket() failed");
        exit (2);
    }

    opt = 32768;
    nn_setsockopt (p->nn_feed, NN_SOL_SOCKET, NN_RCVBUF, &opt, sizeof (opt));
    opt = 1000;
    nn_setsockopt (p->nn_feed, NN_SOL_SOCKET, NN_RECONNECT_IVL, &opt,
        sizeof (opt));
    opt = 1000;
    nn_setsockopt (p->nn_feed, NN_SOL_SOCKET, NN_RECONNECT_IVL_MAX, &opt,
        sizeof (opt));

    for (char **purl = feeds; *purl; ++purl) {
        if (0 > nn_connect (p->nn_feed, *purl)) {
            LOG ("feeder.connect(%s) failed", *purl);
            exit (2);
        }
        else {
            LOG ("feeder.connect(%s)", *purl);
        }
    }
}

static void
proxy_manage_event (proxy_t * p, uint32_t events)
{
    struct sockaddr_in6 from, to;
    socklen_t slen;
    int fd, rc, opt;

    (void) events;
    ASSERT (!(p->flags & FLAG_LISTED));
    ASSERT (!(events & EPOLLOUT));
    ASSERT (!(events & (EPOLLHUP | EPOLLERR)));
    if (!p->events)
        return;

retry:
    slen = sizeof (from);
    fd = accept4 (p->sock_front, SA (&from), &slen,
        SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
        if (errno == EINTR)
            goto retry;
        ASSERT (errno == EAGAIN);
        return proxy_register (p);
    }

    tunnel_t *t = tunnel_reserve (p);

    t->front.sock = fd;

    // The proxy front socket is maybe still active. Then instead of
    // systematically sending the proxy in ACTIVE, check if the limit
    // has been reached. It it is, re-monitor for only errors.
    if (p->pipes.max == ++(p->pipes.count))
        proxy_pause (p);
    else
        proxy_resume (p);

    // Poll a backend
    void *buf = NULL;

    rc = nn_recv (p->nn_feed, &buf, NN_MSG, NN_DONTWAIT);
    if (rc < 0) {
        // TODO better manage the backend's starvation (e.g. retry)
        return tunnel_abort (t, "backend starvation: (%d) %s",
            nn_errno (), nn_strerror (nn_errno ()));
    }
    else if (rc > 128) {
        nn_freemsg (buf);
        return tunnel_abort (t, "invalid backend: %s", "URL too big");
    }
    else {
        char *sto = alloca (rc + 1);

        memcpy (sto, buf, rc);
        sto[rc] = 0;
        rc = sockaddr_init (SA (&to), sto);
        nn_freemsg (buf);
        if (!rc)
            return tunnel_abort (t, "invalid backend: %s", "bad URL");
        char sfrom[64];

        sockaddr_dump (SA (&from), sfrom, sizeof (sfrom));
        ACCESS ("%llu %s -> %s", t->id, sfrom, sto);
    }

    // Connect to the polled backend
    t->back.sock =
        socket (SAFAM (&to), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (t->back.sock < 0)
        return tunnel_abort (t, "socket() error: (%d) %s",
            errno, strerror (errno));

    slen = sizeof (struct sockaddr_in6);
    rc = connect (t->back.sock, SA (&to), slen);
    if (0 > rc && errno != EINPROGRESS)
        return tunnel_abort (t, "connect() error: (%d) %s",
            errno, strerror (errno));

    // Tweak the socket options
    if (opt_buffer_size) {
        opt = PIPE_SIZE / 2;
        setsockopt (t->front.sock, SOL_SOCKET, SO_RCVBUF, &opt, sizeof (opt));
        setsockopt (t->back.sock, SOL_SOCKET, SO_RCVBUF, &opt, sizeof (opt));
        opt = PIPE_SIZE;
        setsockopt (t->front.sock, SOL_SOCKET, SO_SNDBUF, &opt, sizeof (opt));
        setsockopt (t->back.sock, SOL_SOCKET, SO_SNDBUF, &opt, sizeof (opt));
    }

    if (opt_chatty_update) {
        sock_set_chatty (t->front.sock, opt_chatty_front);
        sock_set_chatty (t->back.sock, opt_chatty_back);
    }

    errno = 0;
    tunnel_register (t);
}

/* -------------------------------------------------------------------------- */

static void
manage_monitored_items ()
{
    struct epoll_event evt[MAXEVT];
    int rc, to = 0;

retry:
    if (!ACTIVE_STRUCT_NAME (proxy_t) && !ACTIVE_STRUCT_NAME (channel_t))
        to = -1;
    if (0 > (rc = epoll_wait (fd_epoll, evt, MAXEVT, to))) {
        if (errno == EINTR) {
            if (!running)
                return;
            goto retry;
        }
        LOG ("epoll_wait() failed : (%d) %s", errno, strerror (errno));
        exit (-1);
        return;
    }

    ASSERT (count_epoll >= rc);
    count_epoll -= rc;
    for (register int i = 0; i < rc; ++i) {
        struct monitored_s *mon = evt[i].data.ptr;

        ASSERT (ISMONITORED (mon));
        mon->events = evt[i].events;
        mon->flags = SETACT (mon->flags) & ~FLAG_ACTIVITY;
        if (mon->type == PROXY) {
            PREPEND_STRUCT (ACTIVE_STRUCT_NAME (proxy_t), (proxy_t *) mon);
        }
        else {
            PREPEND_STRUCT (ACTIVE_STRUCT_NAME (channel_t), (channel_t *) mon);
        }
    }
}

static void
main_loop (proxy_t * p, char **feeders)
{
    /* Called once per child, there is no need to inherit this from the
     * father process, so we init this here. */
    proxy_init_feeders (p, feeders);
    fd_epoll = epoll_create (8192);
    ASSERT (fd_epoll >= 0);

    if (0 > listen (p->sock_front, front_backlog)) {
        LOG ("front(%d).listen(%d) failed", p->sock_front, front_backlog);
        exit (1);
    }

    proxy_register (p);
    while (running) {
        DEBUG ("--- monitoring loop");
        if (count_epoll)
            manage_monitored_items ();

        /* manage active channels */
        channel_t *chan, *chans = ACTIVE_STRUCT_NAME (channel_t);

        ACTIVE_STRUCT_NAME (channel_t) = NULL;
        while (chans != NULL) {
            SHIFT_STRUCT (chans, chan);
            chan->flags &= ~FLAG_LISTED;
            channel_manage_events (chan, chan->events);
        }

        /* manage active proxies */
        proxy_t *proxy, *proxies = ACTIVE_STRUCT_NAME (proxy_t);

        ACTIVE_STRUCT_NAME (proxy_t) = NULL;
        while (proxies != NULL) {
            SHIFT_STRUCT (proxies, proxy);
            proxy->flags &= ~FLAG_LISTED;
            proxy_manage_event (proxy, proxy->events);
        }

        DRAIN_STRUCT_CALL (tunnel_t);
    }
}

int
main (int argc, char **argv)
{
    if (argc < 3) {
        LOG ("%s FRONT FEED...", argv[0]);
        exit (1);
    }

    char **opts = main_init (argc, argv);
    proxy_t proxy;

    proxy_init (&proxy);
    proxy_init_front (&proxy, *opts);

    void _run ()
    {
        main_loop (&proxy, opts + 1);
    }
    main_run (&_run);

    nn_close (proxy.nn_feed);
    close (proxy.sock_front);
    close (fd_epoll);
    proxy.nn_feed = proxy.sock_front = fd_epoll = -1;
    PURGE_STRUCT_CALL (tunnel_t);
    PURGE_STRUCT_CALL (pipe_t);
    nn_term ();
    return 0;
}
