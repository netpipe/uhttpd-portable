/*
 * uhttpd - Tiny single-threaded httpd - Main component
 *
 *   Copyright (C) 2010 Jo-Philipp Wich <xm@subsignal.org>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#define _XOPEN_SOURCE 500	/* crypt() */

#include "uhttpd.h"
#include "uhttpd-utils.h"
#include "uhttpd-file.h"

#ifdef HAVE_CGI
#include "uhttpd-cgi.h"
#endif

#ifdef HAVE_LUA
#include "uhttpd-lua.h"
#endif

#ifdef HAVE_TLS
#include "uhttpd-tls.h"
#endif
struct addrinfo {
    int     ai_flags;
    int     ai_family;
    int     ai_socktype;
    int     ai_protocol;
    size_t  ai_addrlen;
    struct  sockaddr *ai_addr;
    char    *ai_canonname;     /* canonical name */
    struct  addrinfo *ai_next; /* this struct can form a linked list */
};

static int run = 1;

static void uh_sigterm(int sig)
{
	run = 0;
}

static void uh_config_parse(struct config *conf)
{
	FILE *c;
	char line[512];
	char *col1 = NULL;
	char *col2 = NULL;
	char *eol  = NULL;

	const char *path = conf->file ? conf->file : "/etc/httpd.conf";


	if ((c = fopen(path, "r")) != NULL)
	{
		memset(line, 0, sizeof(line));

		while (fgets(line, sizeof(line) - 1, c))
		{
			if ((line[0] == '/') && (strchr(line, ':') != NULL))
			{
				if (!(col1 = strchr(line, ':')) || (*col1++ = 0) ||
				    !(col2 = strchr(col1, ':')) || (*col2++ = 0) ||
					!(eol = strchr(col2, '\n')) || (*eol++  = 0))
				{
					continue;
				}

				if (!uh_auth_add(line, col1, col2))
				{
					fprintf(stderr,
							"Notice: No password set for user %s, ignoring "
							"authentication on %s\n", col1, line
					);
				}
			}
			else if (!strncmp(line, "I:", 2))
			{
				if (!(col1 = strchr(line, ':')) || (*col1++ = 0) ||
				    !(eol = strchr(col1, '\n')) || (*eol++  = 0))
				{
				   	continue;
				}

				conf->index_file = strdup(col1);
			}
			else if (!strncmp(line, "E404:", 5))
			{
				if (!(col1 = strchr(line, ':')) || (*col1++ = 0) ||
				    !(eol = strchr(col1, '\n')) || (*eol++  = 0))
				{
					continue;
				}

				conf->error_handler = strdup(col1);
			}
#ifdef HAVE_CGI
			else if ((line[0] == '*') && (strchr(line, ':') != NULL))
			{
				if (!(col1 = strchr(line, '*')) || (*col1++ = 0) ||
				    !(col2 = strchr(col1, ':')) || (*col2++ = 0) ||
				    !(eol = strchr(col2, '\n')) || (*eol++  = 0))
				{
					continue;
				}

				if (!uh_interpreter_add(col1, col2))
				{
					fprintf(stderr,
							"Unable to add interpreter %s for extension %s: "
							"Out of memory\n", col2, col1
					);
				}
			}
#endif
		}

		fclose(c);
	}
}

static void uh_listener_cb(struct uloop_fd *u, unsigned int events);

static int uh_socket_bind(fd_set *serv_fds, int *max_fd,
						  const char *host, const char *port,
						  struct addrinfo *hints, int do_tls,
						  struct config *conf)
{
	int sock = -1;
	int yes = 1;
	int status;
	int bound = 0;

	int tcp_ka_idl, tcp_ka_int, tcp_ka_cnt;

	struct listener *l = NULL;
	struct addrinfo *addrs = NULL, *p = NULL;

	if ((status = getaddrinfo(host, port, hints, &addrs)) != 0)
	{
		fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(status));
	}

	/* try to bind a new socket to each found address */
	for (p = addrs; p; p = p->ai_next)
	{
		/* get the socket */
		if ((sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
		{
			perror("socket()");
			goto error;
		}

		/* "address already in use" */
		if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)))
		{
			perror("setsockopt()");
			goto error;
		}

		/* TCP keep-alive */
		if (conf->tcp_keepalive > 0)
		{
			tcp_ka_idl = 1;
			tcp_ka_cnt = 3;
			tcp_ka_int = conf->tcp_keepalive;

			if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) ||
			    setsockopt(sock, SOL_TCP, TCP_KEEPIDLE,  &tcp_ka_idl, sizeof(tcp_ka_idl)) ||
			    setsockopt(sock, SOL_TCP, TCP_KEEPINTVL, &tcp_ka_int, sizeof(tcp_ka_int)) ||
			    setsockopt(sock, SOL_TCP, TCP_KEEPCNT,   &tcp_ka_cnt, sizeof(tcp_ka_cnt)))
			{
			    fprintf(stderr, "Notice: Unable to enable TCP keep-alive: %s\n",
			    	strerror(errno));
			}
		}

		/* required to get parallel v4 + v6 working */
		if (p->ai_family == AF_INET6)
		{
			if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes)) == -1)
			{
				perror("setsockopt()");
				goto error;
			}
		}

		/* bind */
		if (bind(sock, p->ai_addr, p->ai_addrlen) == -1)
		{
			perror("bind()");
			goto error;
		}

		/* listen */
		if (listen(sock, UH_LIMIT_CLIENTS) == -1)
		{
			perror("listen()");
			goto error;
		}

		/* add listener to global list */
		if (!(l = uh_listener_add(sock, conf)))
		{
			fprintf(stderr, "uh_listener_add(): Failed to allocate memory\n");
			goto error;
		}

#ifdef HAVE_TLS
		/* init TLS */
		l->tls = do_tls ? conf->tls : NULL;
#endif

		/* add socket to server fd set */
		FD_SET(sock, serv_fds);
		fd_cloexec(sock);
		*max_fd = max(*max_fd, sock);

		l->fd.cb = uh_listener_cb;
		uloop_fd_add(&l->fd, ULOOP_READ | ULOOP_WRITE);

		bound++;
		continue;

		error:
		if (sock > 0)
			close(sock);
	}

	freeaddrinfo(addrs);

	return bound;
}

static struct http_request * uh_http_header_parse(struct client *cl,
												  char *buffer, int buflen)
{
	char *method  = buffer;
	char *path    = NULL;
	char *version = NULL;

	char *headers = NULL;
	char *hdrname = NULL;
	char *hdrdata = NULL;

	int i;
	int hdrcount = 0;

	struct http_request *req = &cl->request;


	/* terminate initial header line */
	if ((headers = strfind(buffer, buflen, "\r\n", 2)) != NULL)
	{
		buffer[buflen-1] = 0;

		*headers++ = 0;
		*headers++ = 0;

		/* find request path */
		if ((path = strchr(buffer, ' ')) != NULL)
			*path++ = 0;

		/* find http version */
		if ((path != NULL) && ((version = strchr(path, ' ')) != NULL))
			*version++ = 0;


		/* check method */
		if (strcmp(method, "GET") && strcmp(method, "HEAD") && strcmp(method, "POST"))
		{
			/* invalid method */
			uh_http_response(cl, 405, "Method Not Allowed");
			return NULL;
		}
		else
		{
			switch(method[0])
			{
				case 'G':
					req->method = UH_HTTP_MSG_GET;
					break;

				case 'H':
					req->method = UH_HTTP_MSG_HEAD;
					break;

				case 'P':
					req->method = UH_HTTP_MSG_POST;
					break;
			}
		}

		/* check path */
		if (!path || !strlen(path))
		{
			/* malformed request */
			uh_http_response(cl, 400, "Bad Request");
			return NULL;
		}
		else
		{
			req->url = path;
		}

		/* check version */
		if ((version == NULL) || (strcmp(version, "HTTP/0.9") &&
		    strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1")))
		{
			/* unsupported version */
			uh_http_response(cl, 400, "Bad Request");
			return NULL;
		}
		else
		{
			req->version = strtof(&version[5], NULL);
		}

		D("SRV: %s %s HTTP/%.1f\n",
		  (req->method == UH_HTTP_MSG_POST) ? "POST" :
			(req->method == UH_HTTP_MSG_GET) ? "GET" : "HEAD",
		  req->url, req->version);

		/* process header fields */
		for (i = (int)(headers - buffer); i < buflen; i++)
		{
			/* found eol and have name + value, push out header tuple */
			if (hdrname && hdrdata && (buffer[i] == '\r' || buffer[i] == '\n'))
			{
				buffer[i] = 0;

				/* store */
				if ((hdrcount + 1) < array_size(req->headers))
				{
					D("SRV: HTTP: %s: %s\n", hdrname, hdrdata);

					req->headers[hdrcount++] = hdrname;
					req->headers[hdrcount++] = hdrdata;

					hdrname = hdrdata = NULL;
				}

				/* too large */
				else
				{
					D("SRV: HTTP: header too big (too many headers)\n");
					uh_http_response(cl, 413, "Request Entity Too Large");
					return NULL;
				}
			}

			/* have name but no value and found a colon, start of value */
			else if (hdrname && !hdrdata &&
					 ((i+1) < buflen) && (buffer[i] == ':'))
			{
				buffer[i] = 0;
				hdrdata = &buffer[i+1];

				while ((hdrdata + 1) < (buffer + buflen) && *hdrdata == ' ')
					hdrdata++;
			}

			/* have no name and found [A-Za-z], start of name */
			else if (!hdrname && isalpha(buffer[i]))
			{
				hdrname = &buffer[i];
			}
		}

		/* valid enough */
		req->redirect_status = 200;
		return req;
	}

	/* Malformed request */
	uh_http_response(cl, 400, "Bad Request");
	return NULL;
}


static struct http_request * uh_http_header_recv(struct client *cl)
{
	char *bufptr = cl->httpbuf.buf;
	char *idxptr = NULL;

	ssize_t blen = sizeof(cl->httpbuf.buf)-1;
	ssize_t rlen = 0;

	memset(bufptr, 0, sizeof(cl->httpbuf.buf));

	while (blen > 0)
	{
		/* receive data */
		ensure_out(rlen = uh_tcp_recv(cl, bufptr, blen));
		D("SRV: Client(%d) peek(%d) = %d\n", cl->fd.fd, blen, rlen);

		if (rlen <= 0)
		{
			D("SRV: Client(%d) dead [%s]\n", cl->fd.fd, strerror(errno));
			return NULL;
		}

		blen -= rlen;
		bufptr += rlen;

		if ((idxptr = strfind(cl->httpbuf.buf, sizeof(cl->httpbuf.buf),
							  "\r\n\r\n", 4)))
		{
			/* header read complete ... */
			cl->httpbuf.ptr = idxptr + 4;
			cl->httpbuf.len = bufptr - cl->httpbuf.ptr;

			return uh_http_header_parse(cl, cl->httpbuf.buf,
										(cl->httpbuf.ptr - cl->httpbuf.buf));
		}
	}

	/* request entity too large */
	D("SRV: HTTP: header too big (buffer exceeded)\n");
	uh_http_response(cl, 413, "Request Entity Too Large");

out:
	return NULL;
}

#if defined(HAVE_LUA) || defined(HAVE_CGI)
static int uh_path_match(const char *prefix, const char *url)
{
	if ((strstr(url, prefix) == url) &&
		((prefix[strlen(prefix)-1] == '/') ||
		 (strlen(url) == strlen(prefix))   ||
		 (url[strlen(prefix)] == '/')))
	{
		return 1;
	}

	return 0;
}
#endif

static bool uh_dispatch_request(struct client *cl, struct http_request *req)
{
	struct path_info *pin;
	struct interpreter *ipr = NULL;
	struct config *conf = cl->server->conf;

#ifdef HAVE_LUA
	/* Lua request? */
	if (conf->lua_state &&
		uh_path_match(conf->lua_prefix, req->url))
	{
		return conf->lua_request(cl, conf->lua_state);
	}
	else
#endif

#ifdef HAVE_UBUS
	/* ubus request? */
	if (conf->ubus_state &&
		uh_path_match(conf->ubus_prefix, req->url))
	{
		return conf->ubus_request(cl, conf->ubus_state);
	}
	else
#endif

	/* dispatch request */
	if ((pin = uh_path_lookup(cl, req->url)) != NULL)
	{
		/* auth ok? */
		if (!pin->redirected && uh_auth_check(cl, req, pin))
		{
#ifdef HAVE_CGI
			if (uh_path_match(conf->cgi_prefix, pin->name) ||
				(ipr = uh_interpreter_lookup(pin->phys)) != NULL)
			{
				return uh_cgi_request(cl, pin, ipr);
			}
#endif
			return uh_file_request(cl, pin);
		}
	}

	/* 404 - pass 1 */
	else
	{
		/* Try to invoke an error handler */
		if ((pin = uh_path_lookup(cl, conf->error_handler)) != NULL)
		{
			/* auth ok? */
			if (uh_auth_check(cl, req, pin))
			{
				req->redirect_status = 404;
#ifdef HAVE_CGI
				if (uh_path_match(conf->cgi_prefix, pin->name) ||
					(ipr = uh_interpreter_lookup(pin->phys)) != NULL)
				{
					return uh_cgi_request(cl, pin, ipr);
				}
#endif
				return uh_file_request(cl, pin);
			}
		}

		/* 404 - pass 2 */
		else
		{
			uh_http_sendhf(cl, 404, "Not Found", "No such file or directory");
		}
	}

	return false;
}

static void uh_client_cb(struct uloop_fd *u, unsigned int events);

static void uh_listener_cb(struct uloop_fd *u, unsigned int events)
{
	int new_fd;
	struct listener *serv;
	struct client *cl;
	struct config *conf;

	serv = container_of(u, struct listener, fd);
	conf = serv->conf;

	/* defer client if maximum number of requests is exceeded */
	if (serv->n_clients >= conf->max_requests)
		return;

	/* handle new connections */
	if ((new_fd = accept(u->fd, NULL, 0)) != -1)
	{
		D("SRV: Server(%d) accept => Client(%d)\n", u->fd, new_fd);

		/* add to global client list */
		if ((cl = uh_client_add(new_fd, serv)) != NULL)
		{
			/* add client socket to global fdset */
			uloop_fd_add(&cl->fd, ULOOP_READ | ULOOP_WRITE);

#ifdef HAVE_TLS
			/* setup client tls context */
			if (conf->tls)
			{
				if (conf->tls_accept(cl) < 1)
				{
					D("SRV: Client(%d) SSL handshake failed, drop\n", new_fd);

					/* remove from global client list */
					uh_client_remove(cl);
					return;
				}
			}
#endif

			cl->fd.cb = uh_client_cb;
			fd_cloexec(new_fd);
		}

		/* insufficient resources */
		else
		{
			fprintf(stderr, "uh_client_add(): Cannot allocate memory\n");
			close(new_fd);
		}
	}
}

static void uh_child_cb(struct uloop_process *p, int rv)
{
	struct client *cl = container_of(p, struct client, proc);

	D("SRV: Client(%d) child(%d) is dead\n", cl->fd.fd, cl->proc.pid);

	cl->dead = true;
	cl->fd.eof = true;
	uh_client_cb(&cl->fd, ULOOP_READ | ULOOP_WRITE);
}

static void uh_kill9_cb(struct uloop_timeout *t)
{
	struct client *cl = container_of(t, struct client, timeout);

	if (!kill(cl->proc.pid, 0))
	{
		D("SRV: Client(%d) child(%d) kill(SIGKILL)...\n",
		  cl->fd.fd, cl->proc.pid);

		kill(cl->proc.pid, SIGKILL);
	}
}

static void uh_timeout_cb(struct uloop_timeout *t)
{
	struct client *cl = container_of(t, struct client, timeout);

	D("SRV: Client(%d) child(%d) timed out\n", cl->fd.fd, cl->proc.pid);

	if (!kill(cl->proc.pid, 0))
	{
		D("SRV: Client(%d) child(%d) kill(SIGTERM)...\n",
		  cl->fd.fd, cl->proc.pid);

		kill(cl->proc.pid, SIGTERM);

		cl->timeout.cb = uh_kill9_cb;
		uloop_timeout_set(&cl->timeout, 1000);
	}
}

static void uh_client_cb(struct uloop_fd *u, unsigned int events)
{
	int i;
	struct client *cl;
	struct config *conf;
	struct http_request *req;

	cl = container_of(u, struct client, fd);
	conf = cl->server->conf;

	D("SRV: Client(%d) enter callback\n", u->fd);

	/* undispatched yet */
	if (!cl->dispatched)
	{
		/* we have no headers yet and this was a write event, ignore... */
		if (!(events & ULOOP_READ))
		{
			D("SRV: Client(%d) ignoring write event before headers\n", u->fd);
			return;
		}

		/* attempt to receive and parse headers */
		if (!(req = uh_http_header_recv(cl)))
		{
			D("SRV: Client(%d) failed to receive header\n", u->fd);
			uh_client_shutdown(cl);
			return;
		}

		/* process expect headers */
		foreach_header(i, req->headers)
		{
			if (strcasecmp(req->headers[i], "Expect"))
				continue;

			if (strcasecmp(req->headers[i+1], "100-continue"))
			{
				D("SRV: Client(%d) unknown expect header (%s)\n",
				  u->fd, req->headers[i+1]);

				uh_http_response(cl, 417, "Precondition Failed");
				uh_client_shutdown(cl);
				return;
			}
			else
			{
				D("SRV: Client(%d) sending HTTP/1.1 100 Continue\n", u->fd);

				uh_http_sendf(cl, NULL, "HTTP/1.1 100 Continue\r\n\r\n");
				cl->httpbuf.len = 0; /* client will re-send the body */
				break;
			}
		}

		/* RFC1918 filtering */
		if (conf->rfc1918_filter &&
			sa_rfc1918(&cl->peeraddr) && !sa_rfc1918(&cl->servaddr))
		{
			uh_http_sendhf(cl, 403, "Forbidden",
						   "Rejected request from RFC1918 IP "
						   "to public server address");

			uh_client_shutdown(cl);
			return;
		}

		/* dispatch request */
		if (!uh_dispatch_request(cl, req))
		{
			D("SRV: Client(%d) failed to dispach request\n", u->fd);
			uh_client_shutdown(cl);
			return;
		}

		/* request handler spawned a child, register handler */
		if (cl->proc.pid)
		{
			D("SRV: Client(%d) child(%d) spawned\n", u->fd, cl->proc.pid);

			cl->proc.cb = uh_child_cb;
			uloop_process_add(&cl->proc);

			cl->timeout.cb = uh_timeout_cb;
			uloop_timeout_set(&cl->timeout, conf->script_timeout * 1000);
		}

		/* header processing complete */
		D("SRV: Client(%d) dispatched\n", u->fd);
		cl->dispatched = true;
		return;
	}

	if (!cl->cb(cl))
	{
		D("SRV: Client(%d) response callback signalized EOF\n", u->fd);
		uh_client_shutdown(cl);
		return;
	}
}

#ifdef HAVE_TLS
static inline int uh_inittls(struct config *conf)
{
	/* library handle */
	void *lib;

	/* already loaded */
	if (conf->tls != NULL)
		return 0;

	/* load TLS plugin */
	if (!(lib = dlopen("uhttpd_tls.so", RTLD_LAZY | RTLD_GLOBAL)))
	{
		fprintf(stderr,
				"Notice: Unable to load TLS plugin - disabling SSL support! "
				"(Reason: %s)\n", dlerror()
		);

		return 1;
	}
	else
	{
		/* resolve functions */
		if (!(conf->tls_init   = dlsym(lib, "uh_tls_ctx_init"))      ||
		    !(conf->tls_cert   = dlsym(lib, "uh_tls_ctx_cert"))      ||
		    !(conf->tls_key    = dlsym(lib, "uh_tls_ctx_key"))       ||
		    !(conf->tls_free   = dlsym(lib, "uh_tls_ctx_free"))      ||
		    !(conf->tls_accept = dlsym(lib, "uh_tls_client_accept")) ||
		    !(conf->tls_close  = dlsym(lib, "uh_tls_client_close"))  ||
		    !(conf->tls_recv   = dlsym(lib, "uh_tls_client_recv"))   ||
		    !(conf->tls_send   = dlsym(lib, "uh_tls_client_send")))
		{
			fprintf(stderr,
					"Error: Failed to lookup required symbols "
					"in TLS plugin: %s\n", dlerror()
			);
			exit(1);
		}

		/* init SSL context */
		if (!(conf->tls = conf->tls_init()))
		{
			fprintf(stderr, "Error: Failed to initalize SSL context\n");
			exit(1);
		}
	}

	return 0;
}
#endif

int main (int argc, char **argv)
{
	/* master file descriptor list */
	fd_set serv_fds;

	/* working structs */
	struct addrinfo hints;
	struct sigaction sa;
	struct config conf;

	/* maximum file descriptor number */
	int cur_fd, max_fd = 0;

#ifdef HAVE_TLS
	int tls = 0;
	int keys = 0;
#endif

	int bound = 0;
	int nofork = 0;

	/* args */
	int opt;
	char bind[128];
	char *port = NULL;

#ifdef HAVE_LUA
	/* library handle */
	void *lib;
#endif

	FD_ZERO(&serv_fds);

	/* handle SIGPIPE, SIGINT, SIGTERM */
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);

	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	sa.sa_handler = uh_sigterm;
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	/* prepare addrinfo hints */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags    = 1;//AI_PASSIVE;

	/* parse args */
	memset(&conf, 0, sizeof(conf));
	memset(bind, 0, sizeof(bind));

	uloop_init();

	while ((opt = getopt(argc, argv,
						 "fSDRC:K:E:I:p:s:h:c:l:L:d:r:m:n:x:i:t:T:A:u:U:")) > 0)
	{
		switch(opt)
		{
			/* [addr:]port */
			case 'p':
			case 's':
				if ((port = strrchr(optarg, ':')) != NULL)
				{
					if ((optarg[0] == '[') && (port > optarg) && (port[-1] == ']'))
						memcpy(bind, optarg + 1,
							min(sizeof(bind), (int)(port - optarg) - 2));
					else
						memcpy(bind, optarg,
							min(sizeof(bind), (int)(port - optarg)));

					port++;
				}
				else
				{
					port = optarg;
				}

#ifdef HAVE_TLS
				if (opt == 's')
				{
					if (uh_inittls(&conf))
					{
						fprintf(stderr,
							"Notice: TLS support is disabled, "
							"ignoring '-s %s'\n", optarg
						);
						continue;
					}

					tls = 1;
				}
#endif

				/* bind sockets */
				bound += uh_socket_bind(&serv_fds, &max_fd,
										bind[0] ? bind : NULL,
										port, &hints, (opt == 's'), &conf);

				memset(bind, 0, sizeof(bind));
				break;

#ifdef HAVE_TLS
			/* certificate */
			case 'C':
				if (!uh_inittls(&conf))
				{
					if (conf.tls_cert(conf.tls, optarg) < 1)
					{
						fprintf(stderr,
								"Error: Invalid certificate file given\n");
						exit(1);
					}

					keys++;
				}

				break;

			/* key */
			case 'K':
				if (!uh_inittls(&conf))
				{
					if (conf.tls_key(conf.tls, optarg) < 1)
					{
						fprintf(stderr,
								"Error: Invalid private key file given\n");
						exit(1);
					}

					keys++;
				}

				break;
#endif

			/* docroot */
			case 'h':
				if (! realpath(optarg, conf.docroot))
				{
					fprintf(stderr, "Error: Invalid directory %s: %s\n",
							optarg, strerror(errno));
					exit(1);
				}
				break;

			/* error handler */
			case 'E':
				if ((strlen(optarg) == 0) || (optarg[0] != '/'))
				{
					fprintf(stderr, "Error: Invalid error handler: %s\n",
							optarg);
					exit(1);
				}
				conf.error_handler = optarg;
				break;

			/* index file */
			case 'I':
				if ((strlen(optarg) == 0) || (optarg[0] == '/'))
				{
					fprintf(stderr, "Error: Invalid index page: %s\n",
							optarg);
					exit(1);
				}
				conf.index_file = optarg;
				break;

			/* don't follow symlinks */
			case 'S':
				conf.no_symlinks = 1;
				break;

			/* don't list directories */
			case 'D':
				conf.no_dirlists = 1;
				break;

			case 'R':
				conf.rfc1918_filter = 1;
				break;

			case 'n':
				conf.max_requests = atoi(optarg);
				break;

#ifdef HAVE_CGI
			/* cgi prefix */
			case 'x':
				conf.cgi_prefix = optarg;
				break;

			/* interpreter */
			case 'i':
				if ((optarg[0] == '.') && (port = strchr(optarg, '=')))
				{
					*port++ = 0;
					uh_interpreter_add(optarg, port);
				}
				else
				{
					fprintf(stderr, "Error: Invalid interpreter: %s\n",
							optarg);
					exit(1);
				}
				break;
#endif

#ifdef HAVE_LUA
			/* lua prefix */
			case 'l':
				conf.lua_prefix = optarg;
				break;

			/* lua handler */
			case 'L':
				conf.lua_handler = optarg;
				break;
#endif

#ifdef HAVE_UBUS
			/* ubus prefix */
			case 'u':
				conf.ubus_prefix = optarg;
				break;

			/* ubus socket */
			case 'U':
				conf.ubus_socket = optarg;
				break;
#endif

#if defined(HAVE_CGI) || defined(HAVE_LUA)
			/* script timeout */
			case 't':
				conf.script_timeout = atoi(optarg);
				break;
#endif

			/* network timeout */
			case 'T':
				conf.network_timeout = atoi(optarg);
				break;

			/* tcp keep-alive */
			case 'A':
				conf.tcp_keepalive = atoi(optarg);
				break;

			/* no fork */
			case 'f':
				nofork = 1;
				break;

			/* urldecode */
			case 'd':
				if ((port = malloc(strlen(optarg)+1)) != NULL)
				{
					/* "decode" plus to space to retain compat */
					for (opt = 0; optarg[opt]; opt++)
						if (optarg[opt] == '+')
							optarg[opt] = ' ';
					/* opt now contains strlen(optarg) -- no need to re-scan */
					memset(port, 0, opt+1);
					if (uh_urldecode(port, opt, optarg, opt) < 0)
					    fprintf(stderr, "uhttpd: invalid encoding\n");

					printf("%s", port);
					free(port);
					exit(0);
				}
				break;

			/* basic auth realm */
			case 'r':
				conf.realm = optarg;
				break;

			/* md5 crypt */
			case 'm':
				printf("%s\n", crypt(optarg, "$1$"));
				exit(0);
				break;

			/* config file */
			case 'c':
				conf.file = optarg;
				break;

			default:
				fprintf(stderr,
					"Usage: %s -p [addr:]port [-h docroot]\n"
					"	-f              Do not fork to background\n"
					"	-c file         Configuration file, default is '/etc/httpd.conf'\n"
					"	-p [addr:]port  Bind to specified address and port, multiple allowed\n"
#ifdef HAVE_TLS
					"	-s [addr:]port  Like -p but provide HTTPS on this port\n"
					"	-C file         ASN.1 server certificate file\n"
					"	-K file         ASN.1 server private key file\n"
#endif
					"	-h directory    Specify the document root, default is '.'\n"
					"	-E string       Use given virtual URL as 404 error handler\n"
					"	-I string       Use given filename as index page for directories\n"
					"	-S              Do not follow symbolic links outside of the docroot\n"
					"	-D              Do not allow directory listings, send 403 instead\n"
					"	-R              Enable RFC1918 filter\n"
					"	-n count        Maximum allowed number of concurrent requests\n"
#ifdef HAVE_LUA
					"	-l string       URL prefix for Lua handler, default is '/lua'\n"
					"	-L file         Lua handler script, omit to disable Lua\n"
#endif
#ifdef HAVE_UBUS
					"	-u string       URL prefix for HTTP/JSON handler, default is '/ubus'\n"
					"	-U file         Override ubus socket path\n"
#endif
#ifdef HAVE_CGI
					"	-x string       URL prefix for CGI handler, default is '/cgi-bin'\n"
					"	-i .ext=path    Use interpreter at path for files with the given extension\n"
#endif
#if defined(HAVE_CGI) || defined(HAVE_LUA) || defined(HAVE_UBUS)
					"	-t seconds      CGI, Lua and UBUS script timeout in seconds, default is 60\n"
#endif
					"	-T seconds      Network timeout in seconds, default is 30\n"
					"	-d string       URL decode given string\n"
					"	-r string       Specify basic auth realm\n"
					"	-m string       MD5 crypt given string\n"
					"\n", argv[0]
				);

				exit(1);
		}
	}

#ifdef HAVE_TLS
	if ((tls == 1) && (keys < 2))
	{
		fprintf(stderr, "Error: Missing private key or certificate file\n");
		exit(1);
	}
#endif

	if (bound < 1)
	{
		fprintf(stderr, "Error: No sockets bound, unable to continue\n");
		exit(1);
	}

	/* default docroot */
	if (!conf.docroot[0] && !realpath(".", conf.docroot))
	{
		fprintf(stderr, "Error: Can not determine default document root: %s\n",
			strerror(errno));
		exit(1);
	}

	/* default realm */
	if (!conf.realm)
		conf.realm = "Protected Area";

	/* config file */
	uh_config_parse(&conf);

	/* default max requests */
	if (conf.max_requests <= 0)
		conf.max_requests = 3;

	/* default network timeout */
	if (conf.network_timeout <= 0)
		conf.network_timeout = 30;

#if defined(HAVE_CGI) || defined(HAVE_LUA) || defined(HAVE_UBUS)
	/* default script timeout */
	if (conf.script_timeout <= 0)
		conf.script_timeout = 60;
#endif

#ifdef HAVE_CGI
	/* default cgi prefix */
	if (!conf.cgi_prefix)
		conf.cgi_prefix = "/cgi-bin";
#endif

#ifdef HAVE_LUA
	/* load Lua plugin */
	if (!(lib = dlopen("uhttpd_lua.so", RTLD_LAZY | RTLD_GLOBAL)))
	{
		fprintf(stderr,
				"Notice: Unable to load Lua plugin - disabling Lua support! "
				"(Reason: %s)\n", dlerror());
	}
	else
	{
		/* resolve functions */
		if (!(conf.lua_init    = dlsym(lib, "uh_lua_init"))    ||
		    !(conf.lua_close   = dlsym(lib, "uh_lua_close"))   ||
		    !(conf.lua_request = dlsym(lib, "uh_lua_request")))
		{
			fprintf(stderr,
					"Error: Failed to lookup required symbols "
					"in Lua plugin: %s\n", dlerror()
			);
			exit(1);
		}

		/* init Lua runtime if handler is specified */
		if (conf.lua_handler)
		{
			/* default lua prefix */
			if (!conf.lua_prefix)
				conf.lua_prefix = "/lua";

			conf.lua_state = conf.lua_init(&conf);
		}
	}
#endif

#ifdef HAVE_UBUS
void *lib;
	/* load ubus plugin */
//	if (!(lib = dlopen("./uhttpd_ubus.so", RTLD_LAZY | RTLD_GLOBAL)))
//	{
//		fprintf(stderr,
//				"Notice: Unable to load ubus plugin - disabling ubus support! "
//				"(Reason: %s)\n", dlerror());
//	}
//	else
//	{
//		/* resolve functions */
//		if (!(conf.ubus_init    = dlsym(lib, "uh_ubus_init"))    ||
//		    !(conf.ubus_close   = dlsym(lib, "uh_ubus_close"))   ||
//		    !(conf.ubus_request = dlsym(lib, "uh_ubus_request")))
//		{
//			fprintf(stderr,
//					"Error: Failed to lookup required symbols "
//					"in ubus plugin: %s\n", dlerror()
//			);
//			exit(1);
//		}
//
//		/* default ubus prefix */
//		if (!conf.ubus_prefix)
//			conf.ubus_prefix = "/ubus";
//
//		conf.ubus_state = conf.ubus_init(&conf);
//	}
#endif

	/* fork (if not disabled) */
	if (!nofork)
	{
		switch (fork())
		{
			case -1:
				perror("fork()");
				exit(1);

			case 0:
				/* daemon setup */
				if (chdir("/"))
					perror("chdir()");

				if ((cur_fd = open("/dev/null", O_WRONLY)) > -1)
					dup2(cur_fd, 0);

				if ((cur_fd = open("/dev/null", O_RDONLY)) > -1)
					dup2(cur_fd, 1);

				if ((cur_fd = open("/dev/null", O_RDONLY)) > -1)
					dup2(cur_fd, 2);

				break;

			default:
				exit(0);
		}
	}

	/* server main loop */
	uloop_run();

#ifdef HAVE_LUA
	/* destroy the Lua state */
	if (conf.lua_state != NULL)
		conf.lua_close(conf.lua_state);
#endif

#ifdef HAVE_UBUS
	/* destroy the ubus state */
	if (conf.ubus_state != NULL)
		conf.ubus_close(conf.ubus_state);
#endif

	return 0;
}
