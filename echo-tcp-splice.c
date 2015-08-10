#include <fcntl.h>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>

#include "./utils.h"

static int fd_epoll = -1;
static int front_backlog = 8192;

struct item_s
{
    ssize_t loaded;
    int fd, pfd[2];
    uint32_t events;
    enum
    { SERVER = 1, CLIENT } type:8;
    uint8_t shut;
};

static void
_close (int *pfd)
{
    if (*pfd > 0) {
        close (*pfd);
        *pfd = -1;
    }
}

static void
item_init (struct item_s *it)
{
    ASSERT (it != NULL);
    it->loaded = 0;
    it->fd = it->pfd[0] = it->pfd[1] = -1;
    it->events = EPOLLERR;
    it->type = 0;
    it->shut = 0;
}

static void
item_free (struct item_s *it)
{
    ASSERT (it != NULL);
    _close (&(it->fd));
    _close (it->pfd + 0);
    _close (it->pfd + 1);
    item_init (it);
    free (it);
}

static void
manage_client_event (struct item_s *it, uint32_t evt)
{
    ssize_t rc;

    if (evt & EPOLLIN) {
        rc = splice (it->fd, NULL, it->pfd[1], NULL, PIPE_SIZE,
            SPLICE_F_MOVE | SPLICE_F_MORE | SPLICE_F_NONBLOCK);
        if (rc > 0) {
            it->loaded += rc;
            evt |= EPOLLOUT;
        }
        else if (rc == 0)
            evt |= EPOLLHUP;
        else {
            if (errno != EINTR && errno != EAGAIN)
                evt |= EPOLLERR;
        }
    }
    if (evt & EPOLLOUT) {
        if (it->loaded > 0) {
            rc = splice (it->pfd[0], NULL, it->fd, NULL, it->loaded,
                SPLICE_F_MOVE | SPLICE_F_MORE | SPLICE_F_NONBLOCK);
            if (rc > 0)
                it->loaded -= rc;
            else if (rc < 0) {
                if (errno != EINTR && errno != EAGAIN)
                    evt |= EPOLLERR;
            }
        }
    }
    if ((evt & EPOLLHUP) && !(evt & EPOLLERR)) {
        shutdown (it->fd, SHUT_WR);
        it->shut = 1;
        evt |= EPOLLERR;
    }
    if (evt & EPOLLERR) {
retry_del:
        rc = epoll_ctl (fd_epoll, EPOLL_CTL_DEL, it->fd, NULL);
        if (rc < 0) {
            if (errno == EINTR)
                goto retry_del;
            if (errno != ENOENT) {
                ASSERT (rc == 0);
            }
        }

        return item_free (it);
    }
    else {
        uint32_t e = ((it->loaded > 0) ? EPOLLOUT : 0)
            | ((it->loaded < PIPE_SIZE) ? EPOLLIN : 0);

        if (e != it->events) {
            struct epoll_event epevt;

retry_mod:
            epevt.data.ptr = it;
            epevt.events = e;
            it->events = e;
            rc = epoll_ctl (fd_epoll, EPOLL_CTL_MOD, it->fd, &epevt);
            if (rc < 0) {
                if (errno == EINTR)
                    goto retry_mod;
                ASSERT (rc == 0);
            }
        }
    }
}

static void
manage_server_event (struct item_s *it, uint32_t evt)
{
    struct sockaddr_storage ss;
    int cli, rc, opt;

    ASSERT (evt & EPOLLIN);
    (void) evt;
    socklen_t sslen = sizeof (ss);

    cli = accept4 (it->fd, SA (&ss), &sslen, O_NONBLOCK | O_CLOEXEC);
    if (cli >= 0) {

        opt = PIPE_SIZE / 2;
        setsockopt (cli, SOL_SOCKET, SO_RCVBUF, &opt, sizeof (opt));
        opt = PIPE_SIZE;
        setsockopt (cli, SOL_SOCKET, SO_SNDBUF, &opt, sizeof (opt));

        sock_set_chatty (cli, 1);

        struct item_s *c = malloc (sizeof (struct item_s));

        c->loaded = 0;
        c->pfd[0] = c->pfd[1] = -1;
        if (0 > (rc = pipe2 (c->pfd, O_NONBLOCK | O_CLOEXEC))) {
            (void) close (cli);
            free (c);
            return;
        }
        c->fd = cli;
        c->events = EPOLLIN;
        c->type = CLIENT;
        c->shut = 0;
        fcntl (c->pfd[1], F_SETPIPE_SZ, PIPE_SIZE);

        struct epoll_event evt;

retry_add:
        evt.data.ptr = c;
        evt.events = EPOLLIN;
        rc = epoll_ctl (fd_epoll, EPOLL_CTL_ADD, cli, &evt);
        if (rc < 0) {
            if (rc == EINTR)
                goto retry_add;
            abort ();
        }
    }
}

static void
manage_item_event (struct item_s *it, uint32_t evt)
{
    ASSERT (it != NULL);
    if (it->type == SERVER)
        return manage_server_event (it, evt);
    if (it->type == CLIENT)
        return manage_client_event (it, evt);
    assert (0);
}

static void
main_loop ()
{
    struct epoll_event evt[MAXEVT];

    while (running) {
        memset (evt, 0, sizeof (evt));
        int rc = epoll_wait (fd_epoll, evt, MAXEVT, -1);

        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        else {
            for (int i = 0; i < rc; ++i) {
                struct item_s *it = evt[i].data.ptr;
                uint32_t e = evt[i].events;

                evt[i].data.ptr = NULL;
                evt[i].events = 0;
                manage_item_event (it, e);
            }
        }
    }
}

static void
main_init_srv (char **urlv)
{
    int rc, opt;
    struct sockaddr_storage ss;

    for (char **pu = urlv; *pu; ++pu) {
        struct item_s *srv = malloc (sizeof (struct item_s));

        item_init (srv);

        if (!sockaddr_init (SA (&ss), *pu))
            abort ();
        srv->events = EPOLLIN;
        srv->type = SERVER;
        srv->fd = socket (SAFAM (&ss), SOCK_STREAM | O_CLOEXEC | O_NONBLOCK, 0);
        ASSERT (srv->fd >= 0);

        opt = 1;
        setsockopt (srv->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof (opt));

        rc = bind (srv->fd, SA (&ss), SALEN (&ss));
        ASSERT (rc == 0);
        rc = listen (srv->fd, front_backlog);
        ASSERT (rc == 0);

        struct epoll_event evt;

retry_add:
        evt.data.ptr = srv;
        evt.events = EPOLLIN;
        rc = epoll_ctl (fd_epoll, EPOLL_CTL_ADD, srv->fd, &evt);
        if (rc < 0) {
            if (errno == EINTR)
                goto retry_add;
            ASSERT (rc == 0);
        }
    }
}

int
main (int argc, char **argv)
{
    char **opts = main_init (argc, argv);

    fd_epoll = epoll_create (1024);
    ASSERT (fd_epoll >= 0);
    main_init_srv (opts);
    main_run (&main_loop);
    close (fd_epoll);
    return 0;
}
