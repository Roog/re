/**
 * @file tcp.c  Transport Control Protocol
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_IO_H
#include <io.h>
#endif
#if !defined(WIN32) && !defined (CYGWIN)
#define __USE_POSIX 1  /**< Use POSIX flag */
#define __USE_MISC 1
#include <netdb.h>
#endif
#ifdef __APPLE__
#include "TargetConditionals.h"
#endif
#include <string.h>
#include <re_types.h>
#include <re_fmt.h>
#include <re_mem.h>
#include <re_mbuf.h>
#include <re_list.h>
#include <re_main.h>
#include <re_sa.h>
#include <re_net.h>
#include <re_tcp.h>


#define DEBUG_MODULE "tcp"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/** Platform independent buffer type cast */
#ifdef WIN32
#define BUF_CAST (char *)
#define SOK_CAST (int)
#define SIZ_CAST (int)
#define close closesocket
#elif defined (__SYMBIAN32__)
#define BUF_CAST (void *)
#define SOK_CAST
#define SIZ_CAST
#else
#define BUF_CAST
#define SOK_CAST
#define SIZ_CAST
#endif


enum {
	TCP_RXSZ_DEFAULT = 8192
};


/** Defines a listening TCP socket */
struct tcp_sock {
	int fd;               /**< Listening file descriptor         */
	int fdc;              /**< Cached connection file descriptor */
	tcp_conn_h *connh;    /**< TCP Connect handler               */
	void *arg;            /**< Handler argument                  */
};


/** Defines a TCP connection */
struct tcp_conn {
	struct list helpers;  /**< List of TCP-helpers               */
	struct list sendq;    /**< Sending queue                     */
	int fdc;              /**< Connection file descriptor        */
	tcp_estab_h *estabh;  /**< Connection established handler    */
	tcp_send_h *sendh;    /**< Data send handler                 */
	tcp_recv_h *recvh;    /**< Data receive handler              */
	tcp_close_h *closeh;  /**< Connection close handler          */
	void *arg;            /**< Handler argument                  */
	size_t rxsz;          /**< Maximum receive chunk size        */
	bool active;          /**< We are connecting flag            */
	bool connected;       /**< Connection is connected flag      */
};


struct tcp_helper {
	struct le le;
	tcp_helper_estab_h *estabh;
	tcp_helper_send_h *sendh;
	tcp_helper_recv_h *recvh;
	void *arg;
};


struct tcp_qent {
	struct le le;
	struct mbuf mb;
};


static void tcp_recv_handler(int flags, void *arg);


static bool helper_estab_handler(int *err, bool active, void *arg)
{
	(void)err;
	(void)active;
	(void)arg;
	return false;
}


static bool helper_send_handler(int *err, struct mbuf *mb, void *arg)
{
	(void)err;
	(void)mb;
	(void)arg;
	return false;
}


static bool helper_recv_handler(int *err, struct mbuf *mb, bool *estab,
				void *arg)
{
	(void)err;
	(void)mb;
	(void)estab;
	(void)arg;
	return false;
}


static void sock_destructor(void *data)
{
	struct tcp_sock *ts = data;

	if (ts->fd >= 0) {
		fd_close(ts->fd);
		(void)close(ts->fd);
	}
	if (ts->fdc >= 0)
		(void)close(ts->fdc);
}


static void conn_destructor(void *data)
{
	struct tcp_conn *tc = data;

	list_flush(&tc->helpers);
	list_flush(&tc->sendq);

	if (tc->fdc >= 0) {
		fd_close(tc->fdc);
		(void)close(tc->fdc);
	}
}


static void helper_destructor(void *data)
{
	struct tcp_helper *th = data;

	list_unlink(&th->le);
}


static void qent_destructor(void *arg)
{
	struct tcp_qent *qe = arg;

	list_unlink(&qe->le);
	mem_deref(qe->mb.buf);
}


static int enqueue(struct tcp_conn *tc, struct mbuf *mb, size_t skip)
{
	struct tcp_qent *qe;
	int err;

	if (!tc->sendq.head && !tc->sendh) {

		err = fd_listen(tc->fdc, FD_READ | FD_WRITE,
				tcp_recv_handler, tc);
		if (err)
			return err;
	}

	qe = mem_zalloc(sizeof(*qe), qent_destructor);
	if (!qe)
		return ENOMEM;

	list_append(&tc->sendq, &qe->le, qe);

	mbuf_init(&qe->mb);

	mb->pos += skip;
	err = mbuf_write_mem(&qe->mb, mbuf_buf(mb), mbuf_get_left(mb));
	qe->mb.pos = 0;
	mb->pos -= skip;

	if (err)
		mem_deref(qe);

	return err;
}


static int dequeue(struct tcp_conn *tc)
{
	struct tcp_qent *qe = list_ledata(tc->sendq.head);
	ssize_t n;
#ifdef MSG_NOSIGNAL
	const int flags = MSG_NOSIGNAL; /* disable SIGPIPE signal */
#else
	const int flags = 0;
#endif
	if (!qe) {
		if (tc->sendh)
			tc->sendh(tc->arg);

		return 0;
	}

	n = send(tc->fdc, BUF_CAST mbuf_buf(&qe->mb),
		 qe->mb.end - qe->mb.pos, flags);
	if (n < 0) {
		if (EAGAIN == errno)
			return 0;
#ifdef WIN32
		if (WSAEWOULDBLOCK == WSAGetLastError())
			return 0;
#endif
		return errno;
	}

	qe->mb.pos += n;

	if (qe->mb.pos >= qe->mb.end)
		mem_deref(qe);

	return 0;
}


static void conn_close(struct tcp_conn *tc, int err)
{
	/* Stop polling */
	if (tc->fdc >= 0)
		fd_close(tc->fdc);

	if (tc->closeh)
		tc->closeh(err, tc->arg);
}


static void tcp_recv_handler(int flags, void *arg)
{
	struct tcp_conn *tc = arg;
	struct mbuf *mb = NULL;
	bool hlp_estab = false;
	struct le *le;
	ssize_t n;
	int err;
	socklen_t err_len = sizeof(err);

	if (flags & FD_EXCEPT) {
		DEBUG_INFO("recv handler: got FD_EXCEPT on fd=%d\n", tc->fdc);
	}

	/* check for any errors */
	if (-1 == getsockopt(tc->fdc, SOL_SOCKET, SO_ERROR,
			     BUF_CAST &err, &err_len)) {
		DEBUG_WARNING("recv handler: getsockopt: (%s)\n",
			      strerror(errno));
		return;
	}

	if (err) {
		conn_close(tc, err);
		return;
	}
#if 0
	if (EINPROGRESS != err && EALREADY != err) {
		DEBUG_WARNING("recv handler: Socket error (%s)\n",
			      strerror(err));
		return;
	}
#endif

	if (flags & FD_WRITE) {

		if (tc->connected) {

			err = dequeue(tc);
			if (err) {
				conn_close(tc, err);
				return;
			}

			if (!tc->sendq.head && !tc->sendh) {

				err = fd_listen(tc->fdc, FD_READ,
						tcp_recv_handler, tc);
				if (err) {
					conn_close(tc, err);
					return;
				}
			}

			if (flags & FD_READ)
				goto read;

			return;
		}

		err = fd_listen(tc->fdc, FD_READ, tcp_recv_handler, tc);
		if (err) {
			DEBUG_WARNING("recv handler: fd_listen(): %s\n",
				      strerror(err));
			conn_close(tc, err);
			return;
		}

		le = tc->helpers.head;
		while (le) {
			struct tcp_helper *th = le->data;

			le = le->next;

			if (th->estabh(&err, tc->active, th->arg) || err) {
				if (err)
					conn_close(tc, err);
				return;
			}
		}

		if (tc->estabh)
			tc->estabh(tc->arg);

		tc->connected = true;
		return;
	}

 read:
	mb = mbuf_alloc(tc->rxsz);
	if (!mb)
		return;

	n = recv(tc->fdc, BUF_CAST mb->buf, mb->size, 0);
	if (0 == n) {
		mem_deref(mb);
		conn_close(tc, 0);
		return;
	}
	else if (n < 0) {
		DEBUG_WARNING("recv handler: recv(): %s\n", strerror(errno));
		goto out;
	}

	mb->end = n;

	le = tc->helpers.head;
	while (le) {
		struct tcp_helper *th = le->data;
		bool hdld;

		le = le->next;

		if (!hlp_estab)
		        hdld = th->recvh(&err, mb, &hlp_estab, th->arg);
		else
			hdld = th->estabh(&err, tc->active, th->arg);

		if (hdld || err) {
			if (err)
				conn_close(tc, err);
			goto out;
		}
	}

	mbuf_trim(mb);

	if (!hlp_estab) {
		if (tc->recvh)
			tc->recvh(mb, tc->arg);
	}
	else {
		if (tc->estabh)
			tc->estabh(tc->arg);
	}

 out:
	mem_deref(mb);
}


static struct tcp_conn *conn_alloc(tcp_estab_h *eh, tcp_recv_h *rh,
				   tcp_close_h *ch, void *arg)
{
	struct tcp_conn *tc;

	tc = mem_zalloc(sizeof(*tc), conn_destructor);
	if (!tc)
		return NULL;

	list_init(&tc->helpers);

	tc->fdc    = -1;
	tc->rxsz   = TCP_RXSZ_DEFAULT;
	tc->estabh = eh;
	tc->recvh  = rh;
	tc->closeh = ch;
	tc->arg    = arg;

	return tc;
}


static void tcp_sockopt_set(int fd)
{
#ifdef SO_LINGER
	const struct linger dl = {0, 0};
	int err;

	err = setsockopt(fd, SOL_SOCKET, SO_LINGER, BUF_CAST &dl, sizeof(dl));
	if (err) {
		DEBUG_WARNING("sockopt: SO_LINGER (%s)\n", strerror(err));
	}
#else
	(void)fd;
#endif
}


/**
 * Handler for incoming TCP connections.
 *
 * @param flags  Event flags.
 * @param arg    Handler argument.
 */
static void tcp_conn_handler(int flags, void *arg)
{
	struct sa peer;
	struct tcp_sock *ts = arg;
	int err;

	(void)flags;

	sa_init(&peer, AF_UNSPEC);

	ts->fdc = SOK_CAST accept(ts->fd, &peer.u.sa, &peer.len);
	if (-1 == ts->fdc) {

#if TARGET_OS_IPHONE
		if (EAGAIN == errno) {

			struct tcp_sock *ts_new;
			struct sa laddr;

			err = tcp_sock_local_get(ts, &laddr);
			if (err)
				return;

			if (ts->fd >= 0) {
				fd_close(ts->fd);
				(void)close(ts->fd);
				ts->fd = -1;
			}

			err = tcp_listen(&ts_new, &laddr, NULL, NULL);
			if (err)
				return;

			ts->fd = ts_new->fd;
			ts_new->fd = -1;

			mem_deref(ts_new);

			fd_listen(ts->fd, FD_READ, tcp_conn_handler, ts);
		}
#endif

		return;
	}

	err = net_sockopt_blocking_set(ts->fdc, false);
	if (err) {
		DEBUG_WARNING("conn handler: nonblock set: %s\n",
			      strerror(err));
		(void)close(ts->fdc);
		ts->fdc = -1;
		return;
	}

	tcp_sockopt_set(ts->fdc);

	if (ts->connh)
		ts->connh(&peer, ts->arg);
}


/**
 * Create a TCP Socket
 *
 * @param tsp   Pointer to returned TCP Socket
 * @param local Local listen address (NULL for any)
 * @param ch    Incoming connection handler
 * @param arg   Handler argument
 *
 * @return 0 if success, otherwise errorcode
 */
int tcp_sock_alloc(struct tcp_sock **tsp, const struct sa *local,
		   tcp_conn_h *ch, void *arg)
{
	struct addrinfo hints, *res = NULL, *r;
	char addr[NET_ADDRSTRLEN] = "";
	char serv[6] = "0";
	struct tcp_sock *ts = NULL;
	int error, err;

	if (!tsp)
		return EINVAL;

	ts = mem_zalloc(sizeof(*ts), sock_destructor);
	if (!ts)
		return ENOMEM;

	ts->fd  = -1;
	ts->fdc = -1;

	if (local) {
		err = sa_ntop(local, addr, sizeof(addr));
		(void)re_snprintf(serv, sizeof(serv), "%u", sa_port(local));
		if (err)
			goto out;
	}

	memset(&hints, 0, sizeof(hints));
	/* set-up hints structure */
	hints.ai_family   = PF_UNSPEC;
	hints.ai_flags    = AI_PASSIVE | AI_NUMERICHOST;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	error = getaddrinfo(addr[0] ? addr : NULL, serv, &hints, &res);
	if (error) {
#ifdef WIN32
		DEBUG_WARNING("listen: getaddrinfo: wsaerr=%d\n",
			      WSAGetLastError());
#endif
		DEBUG_WARNING("listen: getaddrinfo: %s:%s error=%d (%s)\n",
			      addr, serv, error, gai_strerror(error));
		err = EADDRNOTAVAIL;
		goto out;
	}

	err = EINVAL;
	for (r = res; r; r = r->ai_next) {
		int fd = -1;

		if (ts->fd > 0)
			continue;

		fd = SOK_CAST socket(r->ai_family, SOCK_STREAM, IPPROTO_TCP);
		if (fd < 0) {
			err = errno;
			continue;
		}

		(void)net_sockopt_reuse_set(fd, true);

		err = net_sockopt_blocking_set(fd, false);
		if (err) {
			DEBUG_WARNING("listen: nonblock set: %s\n",
				      strerror(err));
			(void)close(fd);
			continue;
		}

		tcp_sockopt_set(fd);

		/* OK */
		ts->fd = fd;
		err = 0;
		break;
	}

	freeaddrinfo(res);

	if (-1 == ts->fd)
		goto out;

	ts->connh = ch;
	ts->arg   = arg;

 out:
	if (err)
		mem_deref(ts);
	else
		*tsp = ts;

	return err;
}


/**
 * Bind to a TCP Socket
 *
 * @param ts    TCP Socket
 * @param local Local bind address
 *
 * @return 0 if success, otherwise errorcode
 */
int tcp_sock_bind(struct tcp_sock *ts, const struct sa *local)
{
	struct addrinfo hints, *res = NULL, *r;
	char addr[NET_ADDRSTRLEN] = "";
	char serv[NI_MAXSERV] = "0";
	int error, err;

	if (!ts || ts->fd<0)
		return EINVAL;

	if (local) {
		err = sa_ntop(local, addr, sizeof(addr));
		(void)re_snprintf(serv, sizeof(serv), "%u", sa_port(local));
		if (err)
			return err;
	}

	memset(&hints, 0, sizeof(hints));
	/* set-up hints structure */
	hints.ai_family   = PF_UNSPEC;
	hints.ai_flags    = AI_PASSIVE | AI_NUMERICHOST;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	error = getaddrinfo(addr[0] ? addr : NULL, serv, &hints, &res);
	if (error) {
#ifdef WIN32
		DEBUG_WARNING("sock_bind: getaddrinfo: wsaerr=%d\n",
			      WSAGetLastError());
#endif
		DEBUG_WARNING("sock_bind: getaddrinfo: %s:%s error=%d (%s)\n",
			      addr, serv, error, gai_strerror(error));
		return EADDRNOTAVAIL;
	}

	err = EINVAL;
	for (r = res; r; r = r->ai_next) {

		if (bind(ts->fd, r->ai_addr, SIZ_CAST r->ai_addrlen) < 0) {
			err = errno;
			DEBUG_WARNING("sock_bind: bind: %s (af=%d, %J)\n",
				      strerror(err), r->ai_family, local);
			continue;
		}

		/* OK */
		err = 0;
		break;
	}

	freeaddrinfo(res);

	return err;
}


/**
 * Listen on a TCP Socket
 *
 * @param ts       TCP Socket
 * @param backlog  Maximum length the queue of pending connections
 *
 * @return 0 if success, otherwise errorcode
 */
int tcp_sock_listen(struct tcp_sock *ts, int backlog)
{
	int err;

	if (!ts)
		return EINVAL;

	if (ts->fd < 0) {
		DEBUG_WARNING("sock_listen: invalid fd\n");
		return EBADF;
	}

	if (listen(ts->fd, backlog) < 0) {
		err = errno;
		DEBUG_WARNING("sock_listen: listen(): %s\n", strerror(err));
		return err;
	}

	return fd_listen(ts->fd, FD_READ, tcp_conn_handler, ts);
}


/**
 * Accept an incoming TCP Connection
 *
 * @param tcp Returned TCP Connection object
 * @param ts  Corresponding TCP Socket
 * @param eh  TCP Connection Established handler
 * @param rh  TCP Connection Receive data handler
 * @param ch  TCP Connection close handler
 * @param arg Handler argument
 *
 * @return 0 if success, otherwise errorcode
 */
int tcp_accept(struct tcp_conn **tcp, struct tcp_sock *ts, tcp_estab_h *eh,
	       tcp_recv_h *rh, tcp_close_h *ch, void *arg)
{
	struct tcp_conn *tc;
	int err;

	if (!tcp || !ts)
		return EINVAL;

	tc = conn_alloc(eh, rh, ch, arg);
	if (!tc)
		return ENOMEM;

	/* Transfer ownership to TCP connection */
	tc->fdc = ts->fdc;
	ts->fdc = -1;

	err = fd_listen(tc->fdc, FD_READ | FD_WRITE | FD_EXCEPT,
			tcp_recv_handler, tc);
	if (err) {
		DEBUG_WARNING("accept: fd_listen(): %s\n", strerror(err));
	}

	if (err)
		mem_deref(tc);
	else
		*tcp = tc;

	return err;
}


/**
 * Reject an incoming TCP Connection
 *
 * @param ts  Corresponding TCP Socket
 */
void tcp_reject(struct tcp_sock *ts)
{
	if (!ts)
		return;

	if (ts->fdc > 0) {
		(void)close(ts->fdc);
		ts->fdc = -1;
	}
}


/**
 * Allocate a TCP Connection
 *
 * @param tcp  Returned TCP Connection object
 * @param peer Network address of peer
 * @param eh   TCP Connection Established handler
 * @param rh   TCP Connection Receive data handler
 * @param ch   TCP Connection close handler
 * @param arg  Handler argument
 *
 * @return 0 if success, otherwise errorcode
 */
int tcp_conn_alloc(struct tcp_conn **tcp,
		   const struct sa *peer, tcp_estab_h *eh,
		   tcp_recv_h *rh, tcp_close_h *ch, void *arg)
{
	struct tcp_conn *tc;
	struct addrinfo hints, *res = NULL, *r;
	char addr[NET_ADDRSTRLEN];
	char serv[NI_MAXSERV] = "0";
	int error, err;

	if (!tcp || !sa_isset(peer, SA_ALL))
		return EINVAL;

	tc = conn_alloc(eh, rh, ch, arg);
	if (!tc)
		return ENOMEM;

	memset(&hints, 0, sizeof(hints));
	/* set-up hints structure */
	hints.ai_family   = PF_UNSPEC;
	hints.ai_flags    = AI_PASSIVE | AI_NUMERICHOST;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	err = sa_ntop(peer, addr, sizeof(addr));
	(void)re_snprintf(serv, sizeof(serv), "%u", sa_port(peer));
	if (err)
		goto out;

	error = getaddrinfo(addr, serv, &hints, &res);
	if (error) {
		DEBUG_WARNING("connect: getaddrinfo(): (%s)\n",
			      gai_strerror(error));
		err = EADDRNOTAVAIL;
		goto out;
	}

	err = EINVAL;
	for (r = res; r; r = r->ai_next) {

		tc->fdc = SOK_CAST socket(r->ai_family, SOCK_STREAM,
					  IPPROTO_TCP);
		if (tc->fdc < 0) {
			err = errno;
			continue;
		}

		err = net_sockopt_blocking_set(tc->fdc, false);
		if (err) {
			DEBUG_WARNING("connect: nonblock set: %s\n",
				      strerror(err));
			(void)close(tc->fdc);
			tc->fdc = -1;
			continue;
		}

		tcp_sockopt_set(tc->fdc);

		err = 0;
		break;
	}

	freeaddrinfo(res);

 out:
	if (err)
		mem_deref(tc);
	else
		*tcp = tc;

	return err;
}


/**
 * Bind a TCP Connection to a local address
 *
 * @param tc    TCP Connection object
 * @param local Local bind address
 *
 * @return 0 if success, otherwise errorcode
 */
int tcp_conn_bind(struct tcp_conn *tc, const struct sa *local)
{
	struct addrinfo hints, *res = NULL, *r;
	char addr[NET_ADDRSTRLEN] = "";
	char serv[NI_MAXSERV] = "0";
	int error, err;

	if (!tc)
		return EINVAL;

	if (local) {
		err = sa_ntop(local, addr, sizeof(addr));
		(void)re_snprintf(serv, sizeof(serv), "%u", sa_port(local));
		if (err)
			return err;
	}

	memset(&hints, 0, sizeof(hints));
	/* set-up hints structure */
	hints.ai_family   = PF_UNSPEC;
	hints.ai_flags    = AI_PASSIVE | AI_NUMERICHOST;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	error = getaddrinfo(addr[0] ? addr : NULL, serv, &hints, &res);
	if (error) {
		DEBUG_WARNING("conn_bind: getaddrinfo(): (%s)\n",
			      gai_strerror(error));
		return EADDRNOTAVAIL;
	}

	err = EINVAL;
	for (r = res; r; r = r->ai_next) {

		(void)net_sockopt_reuse_set(tc->fdc, true);

		/* bind to local address */
		if (bind(tc->fdc, r->ai_addr, SIZ_CAST r->ai_addrlen) < 0) {

			/* Special case for mingw32/wine */
			if (0 == errno) {
				goto ok;
			}

			err = errno;
			DEBUG_WARNING("conn_bind: bind(): %J: %s\n",
				      local, strerror(err));
			continue;
		}

	ok:
		/* OK */
		err = 0;
		break;
	}

	freeaddrinfo(res);

	if (err) {
		DEBUG_WARNING("conn_bind failed: %J (%s)\n", local,
			      strerror(err));
	}

	return err;
}


/**
 * Connect to a remote peer
 *
 * @param tc   TCP Connection object
 * @param peer Network address of peer
 *
 * @return 0 if success, otherwise errorcode
 */
int tcp_conn_connect(struct tcp_conn *tc, const struct sa *peer)
{
	struct addrinfo hints, *res = NULL, *r;
	char addr[NET_ADDRSTRLEN];
	char serv[NI_MAXSERV];
	int error, err;

	if (!tc || !sa_isset(peer, SA_ALL))
		return EINVAL;

	tc->active = true;

	if (tc->fdc < 0) {
		DEBUG_WARNING("invalid fd\n");
		return EBADF;
	}

	memset(&hints, 0, sizeof(hints));
	/* set-up hints structure */
	hints.ai_family   = PF_UNSPEC;
	hints.ai_flags    = AI_PASSIVE | AI_NUMERICHOST;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	err = sa_ntop(peer, addr, sizeof(addr));
	(void)re_snprintf(serv, sizeof(serv), "%u", sa_port(peer));
	if (err)
		return err;

	error = getaddrinfo(addr, serv, &hints, &res);
	if (error) {
		DEBUG_WARNING("connect: getaddrinfo(): (%s)\n",
			      gai_strerror(error));
		return EADDRNOTAVAIL;
	}

	for (r = res; r; r = r->ai_next) {
		struct sockaddr *sa = r->ai_addr;

	again:
		if (0 == connect(tc->fdc, sa, SIZ_CAST r->ai_addrlen)) {
			err = 0;
			goto out;
		}
		else {
#ifdef WIN32
			/* Special error handling for Windows */
			if (WSAEWOULDBLOCK == WSAGetLastError()) {
				err = 0;
				goto out;
			}
#endif

			/* Special case for mingw32/wine */
			if (0 == errno) {
				err = 0;
				goto out;
			}

			if (EINTR == errno)
				goto again;

			if (EINPROGRESS != errno && EALREADY != errno) {
				tc->fdc = -1;
				err = errno;
				DEBUG_INFO("connect: connect() %J: %s\n",
					   peer, strerror(err));
			}
		}
	}

 out:
	freeaddrinfo(res);

	if (err)
		return err;

	return fd_listen(tc->fdc, FD_READ | FD_WRITE | FD_EXCEPT,
			 tcp_recv_handler, tc);
}


/**
 * Send data on a TCP Connection to a remote peer
 *
 * @param tc TCP Connection
 * @param mb Buffer to send
 *
 * @return 0 if success, otherwise errorcode
 */
int tcp_send(struct tcp_conn *tc, struct mbuf *mb)
{
	struct le *le;
	int err = 0;
	ssize_t n;
#ifdef MSG_NOSIGNAL
	const int flags = MSG_NOSIGNAL; /* disable SIGPIPE signal */
#else
	const int flags = 0;
#endif

	if (!tc || !mb)
		return EINVAL;

	if (!mbuf_get_left(mb)) {
		DEBUG_WARNING("send: empty mbuf (pos=%u end=%u)\n",
			      mb->pos, mb->end);
		return EINVAL;
	}

	/* call helpers in reverse order */
	le = tc->helpers.tail;
	while (le) {
		struct tcp_helper *th = le->data;

		le = le->prev;

		if (th->sendh(&err, mb, th->arg) || err)
			return err;
	}

	if (tc->sendq.head)
		return enqueue(tc, mb, 0);

	n = send(tc->fdc, BUF_CAST mbuf_buf(mb), mb->end - mb->pos, flags);
	if (n < 0) {

		if (EAGAIN == errno)
			return enqueue(tc, mb, 0);

#ifdef WIN32
		if (WSAEWOULDBLOCK == WSAGetLastError())
			return enqueue(tc, mb, 0);
#endif
		err = errno;

		DEBUG_WARNING("send: write(): %s (fdc=%d)\n", strerror(err),
			      tc->fdc);

#ifdef WIN32
		DEBUG_WARNING("WIN32 error: %d\n", WSAGetLastError());
#endif

		return err;
	}

	if ((size_t)n < mb->end - mb->pos)
		return enqueue(tc, mb, n);

	return 0;
}


int tcp_set_send(struct tcp_conn *tc, tcp_send_h *sendh)
{
	if (!tc)
		return EINVAL;

	tc->sendh = sendh;

	if (tc->sendq.head || !sendh)
		return 0;

	return fd_listen(tc->fdc, FD_READ | FD_WRITE, tcp_recv_handler, tc);
}


/**
 * Get local network address of TCP Socket
 *
 * @param ts    TCP Socket
 * @param local Returned local network address
 *
 * @return 0 if success, otherwise errorcode
 */
int tcp_sock_local_get(const struct tcp_sock *ts, struct sa *local)
{
	if (!ts || !local)
		return EINVAL;

	sa_init(local, AF_UNSPEC);

	if (getsockname(ts->fd, &local->u.sa, &local->len) < 0) {
		DEBUG_WARNING("local get: getsockname(): %s\n",
			      strerror(errno));
		return errno;
	}

	return 0;
}


/**
 * Get local network address of TCP Connection
 *
 * @param tc    TCP Connection
 * @param local Returned local network address
 *
 * @return 0 if success, otherwise errorcode
 */
int tcp_conn_local_get(const struct tcp_conn *tc, struct sa *local)
{
	if (!tc || !local)
		return EINVAL;

	sa_init(local, AF_UNSPEC);

	if (getsockname(tc->fdc, &local->u.sa, &local->len) < 0) {
		DEBUG_WARNING("conn local get: getsockname(): %s\n",
			      strerror(errno));
		return errno;
	}

	return 0;
}


/**
 * Get remote peer network address of TCP Connection
 *
 * @param tc    TCP Connection
 * @param peer Returned remote peer network address
 *
 * @return 0 if success, otherwise errorcode
 */
int tcp_conn_peer_get(const struct tcp_conn *tc, struct sa *peer)
{
	if (!tc)
		return EINVAL;

	sa_init(peer, AF_UNSPEC);

	if (getpeername(tc->fdc, &peer->u.sa, &peer->len) < 0) {
		DEBUG_WARNING("conn peer get: getpeername(): %s\n",
			      strerror(errno));
		return errno;
	}

	return 0;
}


/**
 * Set the maximum receive chunk size on a TCP Connection
 *
 * @param tc   TCP Connection
 * @param rxsz Maximum receive chunk size
 */
void tcp_conn_rxsz_set(struct tcp_conn *tc, size_t rxsz)
{
	if (!tc)
		return;

	tc->rxsz = rxsz;
}


int tcp_conn_fd(const struct tcp_conn *tc)
{
	return tc ? tc->fdc : -1;
}


int tcp_register_helper(struct tcp_helper **thp, struct tcp_conn *tc, int *fd,
			tcp_helper_estab_h *eh, tcp_helper_send_h *sh,
			tcp_helper_recv_h *rh, void *arg)
{
	struct tcp_helper *th;

	if (!thp || !tc)
		return EINVAL;

	th = mem_zalloc(sizeof(*th), helper_destructor);
	if (!th)
		return ENOMEM;

	list_append(&tc->helpers, &th->le, th);

	th->estabh = eh ? eh : helper_estab_handler;
	th->sendh  = sh ? sh : helper_send_handler;
	th->recvh  = rh ? rh : helper_recv_handler;
	th->arg = arg;

	if (fd)
		*fd = tc->fdc;

	*thp = th;

	return 0;
}
