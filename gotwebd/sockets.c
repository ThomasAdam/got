/*
 * Copyright (c) 2016, 2019, 2020-2021 Tracey Emery <tracey@traceyemery.net>
 * Copyright (c) 2015 Mike Larkin <mlarkin@openbsd.org>
 * Copyright (c) 2013 David Gwynne <dlg@openbsd.org>
 * Copyright (c) 2013 Florian Obser <florian@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "got_compat.h"

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/un.h>

#include <net/if.h>
#include <netinet/in.h>

#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <pwd.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "got_error.h"
#include "got_opentemp.h"
#include "got_reference.h"
#include "got_repository.h"
#include "got_privsep.h"

#include "gotwebd.h"
#include "tmpl.h"

#define SOCKS_BACKLOG 5
#define MAXIMUM(a, b)	(((a) > (b)) ? (a) : (b))

volatile int client_cnt;

static struct timeval	timeout = { TIMEOUT_DEFAULT, 0 };

static void	 sockets_sighdlr(int, short, void *);
static void	 sockets_shutdown(void);
static void	 sockets_launch(void);
static void	 sockets_purge(struct gotwebd *);
static void	 sockets_accept_paused(int, short, void *);
static void	 sockets_rlimit(int);

static void	 sockets_dispatch_main(int, short, void *);
static int	 sockets_unix_socket_listen(struct gotwebd *, struct socket *);
static int	 sockets_create_socket(struct address *);
static int	 sockets_accept_reserve(int, struct sockaddr *, socklen_t *,
		    int, volatile int *);

static struct socket *sockets_conf_new_socket(struct gotwebd *,
		    int, struct address *);

int cgi_inflight = 0;

void
sockets(struct gotwebd *env, int fd)
{
	struct event	 sighup, sigint, sigusr1, sigchld, sigterm;

	event_init();

	sockets_rlimit(-1);

	if (config_init(env) == -1)
		fatal("failed to initialize configuration");

	if ((env->iev_parent = malloc(sizeof(*env->iev_parent))) == NULL)
		fatal("malloc");
	imsg_init(&env->iev_parent->ibuf, fd);
	env->iev_parent->handler = sockets_dispatch_main;
	env->iev_parent->data = env->iev_parent;
	event_set(&env->iev_parent->ev, fd, EV_READ, sockets_dispatch_main,
	    env->iev_parent);
	event_add(&env->iev_parent->ev, NULL);

	signal(SIGPIPE, SIG_IGN);

	signal_set(&sighup, SIGHUP, sockets_sighdlr, env);
	signal_add(&sighup, NULL);
	signal_set(&sigint, SIGINT, sockets_sighdlr, env);
	signal_add(&sigint, NULL);
	signal_set(&sigusr1, SIGUSR1, sockets_sighdlr, env);
	signal_add(&sigusr1, NULL);
	signal_set(&sigchld, SIGCHLD, sockets_sighdlr, env);
	signal_add(&sigchld, NULL);
	signal_set(&sigterm, SIGTERM, sockets_sighdlr, env);
	signal_add(&sigterm, NULL);

#ifndef PROFILE
	if (pledge("stdio rpath inet recvfd proc exec sendfd unveil",
	    NULL) == -1)
		fatal("pledge");
#endif

	event_dispatch();
	sockets_shutdown();
}

void
sockets_parse_sockets(struct gotwebd *env)
{
	struct address *a;
	struct socket *new_sock = NULL;
	int sock_id = 1;

	TAILQ_FOREACH(a, &env->addresses, entry) {
		new_sock = sockets_conf_new_socket(env, sock_id, a);
		if (new_sock) {
			sock_id++;
			TAILQ_INSERT_TAIL(&env->sockets,
			    new_sock, entry);
		}
	}
}

static struct socket *
sockets_conf_new_socket(struct gotwebd *env, int id, struct address *a)
{
	struct socket *sock;
	struct address *acp;

	if ((sock = calloc(1, sizeof(*sock))) == NULL)
		fatalx("%s: calloc", __func__);

	sock->conf.id = id;
	sock->fd = -1;
	sock->conf.af_type = a->ss.ss_family;

	if (a->ss.ss_family == AF_UNIX) {
		struct sockaddr_un *sun;

		sun = (struct sockaddr_un *)&a->ss;
		if (strlcpy(sock->conf.unix_socket_name, sun->sun_path,
		    sizeof(sock->conf.unix_socket_name)) >=
		    sizeof(sock->conf.unix_socket_name))
			fatalx("unix socket path too long: %s", sun->sun_path);
	}

	sock->conf.fcgi_socket_port = a->port;

	acp = &sock->conf.addr;

	memcpy(&acp->ss, &a->ss, sizeof(acp->ss));
	acp->slen = a->slen;
	acp->ai_family = a->ai_family;
	acp->ai_socktype = a->ai_socktype;
	acp->ai_protocol = a->ai_protocol;
	acp->port = a->port;
	if (*a->ifname != '\0') {
		if (strlcpy(acp->ifname, a->ifname,
		    sizeof(acp->ifname)) >= sizeof(acp->ifname)) {
			fatalx("%s: interface name truncated",
			    __func__);
		}
	}

	return (sock);
}

static void
sockets_launch(void)
{
	struct socket *sock;
	struct server *srv;
	const struct got_error *error;

	TAILQ_FOREACH(sock, &gotwebd_env->sockets, entry) {
		log_debug("%s: configuring socket %d (%d)", __func__,
		    sock->conf.id, sock->fd);

		event_set(&sock->ev, sock->fd, EV_READ | EV_PERSIST,
		    sockets_socket_accept, sock);

		if (event_add(&sock->ev, NULL))
			fatalx("event add sock");

		evtimer_set(&sock->pause, sockets_accept_paused, sock);

		log_debug("%s: running socket listener %d", __func__,
		    sock->conf.id);
	}

	TAILQ_FOREACH(srv, &gotwebd_env->servers, entry) {
		if (unveil(srv->repos_path, "r") == -1)
			fatal("unveil %s", srv->repos_path);
	}

	error = got_privsep_unveil_exec_helpers();
	if (error)
		fatal("%s", error->msg);

	if (unveil(NULL, NULL) == -1)
		fatal("unveil");
}

static void
sockets_purge(struct gotwebd *env)
{
	struct socket *sock, *tsock;

	/* shutdown and remove sockets */
	TAILQ_FOREACH_SAFE(sock, &env->sockets, entry, tsock) {
		if (event_initialized(&sock->ev))
			event_del(&sock->ev);
		if (evtimer_initialized(&sock->evt))
			evtimer_del(&sock->evt);
		if (evtimer_initialized(&sock->pause))
			evtimer_del(&sock->pause);
		if (sock->fd != -1)
			close(sock->fd);
		TAILQ_REMOVE(&env->sockets, sock, entry);
	}
}

static void
sockets_dispatch_main(int fd, short event, void *arg)
{
	struct imsgev		*iev = arg;
	struct imsgbuf		*ibuf;
	struct imsg		 imsg;
	struct gotwebd		*env = gotwebd_env;
	ssize_t			 n;
	int			 shut = 0;

	ibuf = &iev->ibuf;

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read error");
		if (n == 0)	/* Connection closed */
			shut = 1;
	}
	if (event & EV_WRITE) {
		if ((n = msgbuf_write(&ibuf->w)) == -1 && errno != EAGAIN)
			fatal("msgbuf_write");
		if (n == 0)	/* Connection closed */
			shut = 1;
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("imsg_get");
		if (n == 0)	/* No more messages. */
			break;

		switch (imsg.hdr.type) {
		case IMSG_CFG_SRV:
			config_getserver(env, &imsg);
			break;
		case IMSG_CFG_SOCK:
			config_getsock(env, &imsg);
			break;
		case IMSG_CFG_FD:
			config_getfd(env, &imsg);
			break;
		case IMSG_CFG_DONE:
			config_getcfg(env, &imsg);
			break;
		case IMSG_CTL_START:
			sockets_launch();
			break;
		default:
			fatalx("%s: unknown imsg type %d", __func__,
			    imsg.hdr.type);
		}

		imsg_free(&imsg);
	}

	if (!shut)
		imsg_event_add(iev);
	else {
		/* This pipe is dead.  Remove its event handler */
		event_del(&iev->ev);
		event_loopexit(NULL);
	}
}

static void
sockets_sighdlr(int sig, short event, void *arg)
{
	switch (sig) {
	case SIGHUP:
		log_info("%s: ignoring SIGHUP", __func__);
		break;
	case SIGPIPE:
		log_info("%s: ignoring SIGPIPE", __func__);
		break;
	case SIGUSR1:
		log_info("%s: ignoring SIGUSR1", __func__);
		break;
	case SIGCHLD:
		break;
	case SIGINT:
	case SIGTERM:
		sockets_shutdown();
		break;
	default:
		log_info("SIGNAL: %d", sig);
		fatalx("unexpected signal");
	}
}

static void
sockets_shutdown(void)
{
	struct server *srv, *tsrv;
	struct socket *sock, *tsock;

	sockets_purge(gotwebd_env);

	/* clean sockets */
	TAILQ_FOREACH_SAFE(sock, &gotwebd_env->sockets, entry, tsock) {
		TAILQ_REMOVE(&gotwebd_env->sockets, sock, entry);
		close(sock->fd);
		free(sock);
	}

	/* clean servers */
	TAILQ_FOREACH_SAFE(srv, &gotwebd_env->servers, entry, tsrv)
		free(srv);

	free(gotwebd_env);

	exit(0);
}

int
sockets_privinit(struct gotwebd *env, struct socket *sock)
{
	if (sock->conf.af_type == AF_UNIX) {
		log_debug("%s: initializing unix socket %s", __func__,
		    sock->conf.unix_socket_name);
		sock->fd = sockets_unix_socket_listen(env, sock);
		if (sock->fd == -1) {
			log_warnx("%s: create unix socket failed", __func__);
			return -1;
		}
	}

	if (sock->conf.af_type == AF_INET || sock->conf.af_type == AF_INET6) {
		log_debug("%s: initializing %s FCGI socket on port %d",
		    __func__, sock->conf.af_type == AF_INET ? "inet" : "inet6",
		    sock->conf.fcgi_socket_port);
		sock->fd = sockets_create_socket(&sock->conf.addr);
		if (sock->fd == -1) {
			log_warnx("%s: create FCGI socket failed", __func__);
			return -1;
		}
	}

	return 0;
}

static int
sockets_unix_socket_listen(struct gotwebd *env, struct socket *sock)
{
	int u_fd = -1;
	mode_t old_umask, mode;

	u_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK| SOCK_CLOEXEC, 0);
	if (u_fd == -1) {
		log_warn("%s: socket", __func__);
		return -1;
	}

	if (unlink(sock->conf.unix_socket_name) == -1) {
		if (errno != ENOENT) {
			log_warn("%s: unlink %s", __func__,
			    sock->conf.unix_socket_name);
			close(u_fd);
			return -1;
		}
	}

	old_umask = umask(S_IXUSR|S_IXGRP|S_IWOTH|S_IROTH|S_IXOTH);
	mode = S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP;

	if (bind(u_fd, (struct sockaddr *)&sock->conf.addr.ss,
	    sock->conf.addr.slen) == -1) {
		log_warn("%s: bind: %s", __func__, sock->conf.unix_socket_name);
		close(u_fd);
		(void)umask(old_umask);
		return -1;
	}

	(void)umask(old_umask);

	if (chmod(sock->conf.unix_socket_name, mode) == -1) {
		log_warn("%s: chmod", __func__);
		close(u_fd);
		(void)unlink(sock->conf.unix_socket_name);
		return -1;
	}

	if (chown(sock->conf.unix_socket_name, env->pw->pw_uid,
	    env->pw->pw_gid) == -1) {
		log_warn("%s: chown", __func__);
		close(u_fd);
		(void)unlink(sock->conf.unix_socket_name);
		return -1;
	}

	if (listen(u_fd, SOCKS_BACKLOG) == -1) {
		log_warn("%s: listen", __func__);
		return -1;
	}

	return u_fd;
}

static int
sockets_create_socket(struct address *a)
{
	int fd = -1, o_val = 1, flags;

	fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
	if (fd == -1)
		return -1;

	log_debug("%s: opened socket (%d) for %s", __func__,
	    fd, a->ifname);

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &o_val,
	    sizeof(int)) == -1) {
		log_warn("%s: setsockopt error", __func__);
		close(fd);
		return -1;
	}

	/* non-blocking */
	flags = fcntl(fd, F_GETFL);
	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) == -1) {
		log_info("%s: could not enable non-blocking I/O", __func__);
		close(fd);
		return -1;
	}

	if (bind(fd, (struct sockaddr *)&a->ss, a->slen) == -1) {
		close(fd);
		log_info("%s: can't bind to port %d", __func__, a->port);
		return -1;
	}

	if (listen(fd, SOMAXCONN) == -1) {
		log_warn("%s, unable to listen on socket", __func__);
		close(fd);
		return -1;
	}

	return (fd);
}

static int
sockets_accept_reserve(int sockfd, struct sockaddr *addr, socklen_t *addrlen,
    int reserve, volatile int *counter)
{
	int ret;

	if (getdtablecount() + reserve +
	    ((*counter + 1) * FD_NEEDED) >= getdtablesize()) {
		log_debug("inflight fds exceeded");
		errno = EMFILE;
		return -1;
	}
/* TA:  This needs fixing upstream. */
#ifdef __APPLE__
	ret = accept(sockfd, addr, addrlen);
#else
	ret = accept4(sockfd, addr, addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
#endif

	if (ret > -1) {
		(*counter)++;
		log_debug("inflight incremented, now %d", *counter);
	}

	return ret;
}

static void
sockets_accept_paused(int fd, short events, void *arg)
{
	struct socket *sock = (struct socket *)arg;

	event_add(&sock->ev, NULL);
}

void
sockets_socket_accept(int fd, short event, void *arg)
{
	struct socket *sock = (struct socket *)arg;
	struct sockaddr_storage ss;
	struct timeval backoff;
	struct request *c = NULL;
	socklen_t len;
	int s;

	backoff.tv_sec = 1;
	backoff.tv_usec = 0;

	event_add(&sock->ev, NULL);
	if (event & EV_TIMEOUT)
		return;

	len = sizeof(ss);

	s = sockets_accept_reserve(fd, (struct sockaddr *)&ss, &len,
	    FD_RESERVE, &cgi_inflight);

	if (s == -1) {
		switch (errno) {
		case EINTR:
		case EWOULDBLOCK:
		case ECONNABORTED:
			return;
		case EMFILE:
		case ENFILE:
			event_del(&sock->ev);
			evtimer_add(&sock->pause, &backoff);
			return;
		default:
			log_warn("%s: accept", __func__);
		}
	}

	if (client_cnt > GOTWEBD_MAXCLIENTS)
		goto err;

	c = calloc(1, sizeof(struct request));
	if (c == NULL) {
		log_warn("%s", __func__);
		close(s);
		cgi_inflight--;
		return;
	}

	c->tp = template(c, &fcgi_write, c->outbuf, sizeof(c->outbuf));
	if (c->tp == NULL) {
		log_warn("%s", __func__);
		close(s);
		cgi_inflight--;
		free(c);
		return;
	}

	c->fd = s;
	c->sock = sock;
	memcpy(c->priv_fd, sock->priv_fd, sizeof(c->priv_fd));
	c->buf_pos = 0;
	c->buf_len = 0;
	c->request_started = 0;
	c->sock->client_status = CLIENT_CONNECT;

	event_set(&c->ev, s, EV_READ|EV_PERSIST, fcgi_request, c);
	event_add(&c->ev, NULL);

	evtimer_set(&c->tmo, fcgi_timeout, c);
	evtimer_add(&c->tmo, &timeout);

	client_cnt++;

	return;
err:
	cgi_inflight--;
	close(s);
	if (c != NULL)
		free(c);
}

static void
sockets_rlimit(int maxfd)
{
	struct rlimit rl;

	if (getrlimit(RLIMIT_NOFILE, &rl) == -1)
		fatal("%s: failed to get resource limit", __func__);
	log_debug("%s: max open files %llu", __func__,
	    (unsigned long long)rl.rlim_max);

	/*
	 * Allow the maximum number of open file descriptors for this
	 * login class (which should be the class "daemon" by default).
	 */
	if (maxfd == -1)
		rl.rlim_cur = rl.rlim_max;
	else
		rl.rlim_cur = MAXIMUM(rl.rlim_max, (rlim_t)maxfd);
	if (setrlimit(RLIMIT_NOFILE, &rl) == -1)
		fatal("%s: failed to set resource limit", __func__);
}
