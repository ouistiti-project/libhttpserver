/*****************************************************************************
 * httpserver.c: Simple HTTP server
 * this file is part of https://github.com/ouistiti-project/libhttpserver
 *****************************************************************************
 * Copyright (C) 2016-2017
 *
 * Authors: Marc Chalain <marc.chalain@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#if defined(__GNUC__) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE
#else
# define strcasestr strstr
#endif
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/resource.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>

#ifdef USE_POLL
#include <poll.h>
#else
#include <sys/select.h>
#endif

#include <netdb.h>

#include "valloc.h"
#include "vthread.h"
#include "ouistiti/log.h"
#include "ouistiti/httpserver.h"
#include "_httpserver.h"
#include "_httpclient.h"
#include "_httpmessage.h"
#include "_buffer.h"
#include "dbentry.h"

#define buffer_dbg(...)
#define message_dbg(...)
#define client_dbg(...)
#define server_dbg(...)

extern httpserver_ops_t *httpserver_ops;

/********************************************************************/
static http_server_config_t defaultconfig = {
	.addr = NULL,
	.port = 80,
	.maxclients = 10,
	.chunksize = 64,
	.keepalive = 1,
	.version = HTTP11,
};

#ifndef DEFAULTSCHEME
#define DEFAULTSCHEME
const char str_defaultscheme[5] = "http";
#endif
const char str_defaultsservice[4] = "www";
const char str_true[] = "true";
const char str_false[] = "false";

static const char str_session[] = "session";
static const char str_methods[] = "methods";

static char _httpserver_software[] = "libhttpserver";
char *httpserver_software = _httpserver_software;
/***********************************************************************
 * http_server
 */
static int _httpserver_setmod(http_server_t *server, http_client_t *client)
{
	int ret = ESUCCESS;

	for (http_server_mod_t *mod = server->mod; mod; mod = mod->next)
	{
		ret = httpclient_addmodule(client, mod);
	}
	return ret;
}

static int _httpserver_prepare(http_server_t *server)
{
	int count = 0;
	int maxfd = 0;

	int checksockets = 1;
	maxfd = server->sock;

#ifndef VTHREAD
	for (const http_client_t *client = server->clients; client != NULL; client = client->next)
	{
		if (httpclient_socket(client) > 0)
		{
			int status = client->ops->status(client->opsctx);
			if (status == ESUCCESS)
			{
				/**
				 * data already availlables.
				 * short cut the sockets polling to go directly to
				 * the sockets checking.
				 * _httpserver_prepare returns -1 (maxfd = -1)
				 */
				checksockets = 0;
#ifdef USE_POLL
				server->poll_set[server->numfds].revents = POLLIN;
				if (client->request_queue)
				{
					server->poll_set[server->numfds].revents |= POLLOUT;
				}
#endif
			}
#ifdef USE_POLL
			server->poll_set[server->numfds].fd = client->sock;
			server->poll_set[server->numfds].events = POLLIN;
			if (client->request_queue)
			{
				server->poll_set[server->numfds].events |= POLLOUT;
			}
#else
			if (client->request_queue)
			{
				FD_SET(httpclient_socket(client), &server->fds[1]);
			}
			FD_SET(httpclient_socket(client), &server->fds[0]);
			FD_SET(httpclient_socket(client), &server->fds[2]);
#endif
			server->numfds++;

			maxfd = (maxfd > httpclient_socket(client))? maxfd:httpclient_socket(client);
			count++;
			if (count >= server->config->maxclients)
				break;
		}
	}

#else
	//_httpserver_checkclients(server, &rfds, &wfds);
#endif

#ifdef USE_POLL
	if (count < server->config->maxclients)
	{
		server->poll_set[server->numfds].fd = server->sock;
		server->poll_set[server->numfds].events = POLLIN;
	}
#else
	if (count < server->config->maxclients)
		FD_SET(server->sock, &server->fds[0]);
	FD_SET(server->sock, &server->fds[2]);
#endif
	server->numfds++;
	if (!checksockets)
		return -1;
	return maxfd;
}

static http_client_t *_httpserver_removeclient(http_server_t *server, http_client_t *client)
{
#ifdef VTHREAD
	vthread_join(client->thread, NULL);
#endif
	http_client_t *client2 = server->clients;
	if (client == server->clients)
	{
		server->clients = client->next;
		client2 = server->clients;
	}
	else
	{
		while (client2->next != client) client2 = client2->next;
		client2->next = client->next;
		client2 = client2->next;
	}
	httpclient_destroy(client);
	return client2;
}

static int _httpserver_checkclients(http_server_t *server, fd_set *prfds, const fd_set *pwfds, const fd_set *pefds)
{
	int ret = 0;
	http_client_t *client = server->clients;
	while (client != NULL)
	{
#ifdef CHECK_EBADF
		/**
		 * Some ppoll and pselect return with EBADF without explanation.
		 * This code should check all socket and find the wrong socket.
		 * But the result is allway good.
		 */
		if (errno == EBADF)
		{
			if (fcntl(httpclient_socket(client), F_GETFL) < 0)
			{
				err("client %p error (%d, %s)", client, errno, strerror(errno));
				if (errno == EBADF)
				{
					err("EBADF");
					client->state |= CLIENT_STOPPED;
				}
			}
			/** fcntl changes the value of errno **/
			errno = EBADF;
		}
#endif
		if (client->timeout < 0)
		{
			client->state |= CLIENT_STOPPED;
		}
#ifndef VTHREAD
		if (FD_ISSET(httpclient_socket(client), pefds))
		{
			err("client %p exception", client);
			if ((client->state & CLIENT_MACHINEMASK) != CLIENT_NEW)
				client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
			else
				FD_CLR(httpclient_socket(client), prfds);
		}
		if (FD_ISSET(httpclient_socket(client), prfds) ||
			client->request_queue != NULL)
		{
			ret = _httpclient_run(client);
		}

#endif
		if (((client->state & CLIENT_MACHINEMASK) == CLIENT_DEAD)
#ifdef VTHREAD
			|| (!vthread_exist(client->thread))
#endif
			)
		{
			warn("client %p died", client);
			client = _httpserver_removeclient(server, client);
		}
		else
		{
			ret++;
			client = client->next;
		}
	}
	warn("server: %d clients running", ret);

	return ret;
}

static int _httpserver_addclient(http_server_t *server, http_client_t *client)
{
	int ret = EREJECT;

	ret = _httpserver_setmod(server, client);
#ifdef VTHREAD
	if (ret == ESUCCESS)
	{
		vthread_attr_t attr;
		client->state &= ~CLIENT_STOPPED;
		client->state |= CLIENT_STARTED;
		ret = vthread_create(&client->thread, &attr, (vthread_routine)_httpclient_run, (void *)client, sizeof(*client));
		if (!vthread_sharedmemory(client->thread))
		{
			/**
			 * To disallow the reception of SIGPIPE during the
			 * "send" call, the socket into the parent process
			 * must be closed.
			 * Or the tcpserver must disable SIGPIPE
			 * during the sending, but in this case
			 * it is impossible to recceive real SIGPIPE.
			 */
			close(client->sock);
		}
	}
#endif
	if (ret == ESUCCESS)
	{
		client->next = server->clients;
		server->clients = client;
	}
	else
	{
		/**
		 * One module rejected the new client socket.
		 * It may be a bug or a module checking the client
		 * like "clientfilter"
		 */
		warn("server: connection refused by modules");
		httpclient_shutdown(client);
		httpclient_destroy(client);
	}
	return ret;
}

static int _httpserver_checkserver(http_server_t *server, fd_set *prfds, fd_set *pwfds, fd_set *pefds)
{
	int ret = ESUCCESS;

	if (server->sock == -1)
		return EREJECT;

	if (FD_ISSET(server->sock, pefds))
	{
		err("server %p exception", server);
		FD_CLR(server->sock, prfds);
	}

	if (FD_ISSET(server->sock, prfds))
	{
		http_client_t *client = server->ops->createclient(server);

		if (client != NULL)
		{
			_httpserver_addclient(server, client);
			/// the return of this function talk about the client not the server
		}
		else
			warn("server: client connection error");
	}

	return ret;
}

#ifndef VTHREAD
static int _httpserver_connect(http_server_t *server)
{
	/**
	 * TODO: this function will be use
	 * to connect all server socket on the same loop
	 */
	return ESUCCESS;
}
#endif

static int _httpserver_select(http_server_t *server, int maxfd, fd_set *prfds, fd_set *pwfds, fd_set *pefds, struct timespec *ptimeout)
{
	int nbselect = 0;
#ifdef USE_POLL
	if (maxfd > 0)
		//nbselect = ppoll(server->poll_set, server->numfds, ptimeout, NULL);
		nbselect = poll(server->poll_set, server->numfds, WAIT_TIMER * 1000);

	if (nbselect > 0)
	{
		int j;
		for (j = 0; j < server->numfds; j++)
		{
			if (server->poll_set[j].revents & POLLIN)
			{
				FD_SET(server->poll_set[j].fd, &server->fds[0]);
				server->poll_set[j].revents &= ~POLLIN;
			}
			if (server->poll_set[j].revents & POLLOUT)
			{
				FD_SET(server->poll_set[j].fd, &server->fds[1]);
				server->poll_set[j].revents &= ~POLLOUT;
			}
			if (server->poll_set[j].revents & POLLERR)
			{
				FD_SET(server->poll_set[j].fd, &server->fds[0]);
				FD_SET(server->poll_set[j].fd, &server->fds[2]);
			}
			if (server->poll_set[j].revents & POLLHUP)
			{
				if (server->poll_set[j].fd == server->sock)
				{
					nbselect = -1;
					server->run = 0;
					errno = ECONNABORTED;
				}
				else
				{
					FD_SET(server->poll_set[j].fd, &server->fds[0]);
					FD_SET(server->poll_set[j].fd, &server->fds[2]);
				}
				server->poll_set[j].revents &= ~POLLHUP;
			}
			if (server->poll_set[j].revents)
				err("server %p fd %d poll %x", server, server->poll_set[j].fd, server->poll_set[j].revents);
		}
	}
#else
	if (maxfd > 0)
		nbselect = pselect(maxfd +1, &server->fds[0],
					&server->fds[1], &server->fds[2], ptimeout, NULL);
#endif
	return nbselect;
}

static int _httpserver_run(http_server_t *server)
{
	int ret = ESUCCESS;
	int run = 0;

	server->run = 1;
	run = 1;

	warn("server %s %d running", server->hostname.data, server->config->port);
	while(run > 0)
	{
		struct timespec *ptimeout = NULL;
		int maxfd = 0;
		fd_set *prfds, *pwfds, *pefds;

		prfds = &server->fds[0];
		pwfds = &server->fds[1];
		pefds = &server->fds[2];

		FD_ZERO(prfds);
		FD_ZERO(pwfds);
		FD_ZERO(pefds);

#ifndef VTHREAD
		struct timespec timeout;
		if (server->config->keepalive)
		{
			timeout.tv_sec = WAIT_TIMER;
			timeout.tv_nsec = 0;
			ptimeout = &timeout;
		}
#endif

		server->numfds = 0;
		int lastfd = _httpserver_prepare(server);
		if (lastfd > 0)
			maxfd = (maxfd > lastfd)?maxfd:lastfd;
		else
			maxfd = lastfd;

		int nbselect = _httpserver_select(server, maxfd, prfds, pwfds, pefds, ptimeout);

		server_dbg("server: events %d", nbselect);
		if (nbselect == 0)
		{
#ifdef VTHREAD
			//vthread_yield(server->thread);
#else
			/**
			 * poll/select exit on timeout
			 * Check if a client is still available
			 */
			int checkclients = 0;
			for (http_client_t *client = server->clients; client != NULL; client = client->next)
			{
				client->timeout -= WAIT_TIMER * 100;
				if (client->timeout < 0)
				{
					checkclients = 1;
					break;
				}
			}
			if (checkclients)
				_httpserver_checkclients(server, prfds, pwfds, pefds);
#endif
		}
		else if (nbselect < 0)
		{
			if (errno == EINTR)
			{
				warn("server %p select error (%d, %s)", server, errno, strerror(errno));
				errno = 0;
				server->run = 0;
			}
			else if (errno == EAGAIN)
			{
				errno = 0;
			}
			/**
			 * Some time receives error
			 *    ENOTCONN 107 Transport Endpoint not connected
			 *    EBADF 9 Bad File descriptor
			 * without explanation.
			 */
			else if (errno == EBADF || errno == ENOTCONN)
			{
				warn("server %p select error (%d, %s)", server, errno, strerror(errno));
#ifdef CHECK_EBADF
				if (fcntl(server->sock, F_GETFL) < 0)
				{
					server->run = 0;
					err("server %p select EBADF", server);
					ret = EREJECT;
				}
				else
				{
					for (http_client_t *client = server->clients; client != NULL; client = client->next)
					{
						warn("EBADF %p (%d)", client, client->sock);
						int ret = write(client->sock, NULL, 0);
						warn("EBADF %p (%d)", client, ret);
					}
				}
#endif
				errno = 0;
			}
			else
			{
				err("server %p select error (%d, %s)", server, errno, strerror(errno));
				server->run = 0;
				ret = EREJECT;
			}
		}
		else if (nbselect > 0)
		{
			int count = 0;
			count = _httpserver_checkclients(server, prfds, pwfds, pefds);
			if (count < server->config->maxclients)
				ret = _httpserver_checkserver(server, prfds, pwfds, pefds);
			else
			{
				ret = EINCOMPLETE;
				warn("server: too many clients");
#ifdef VTHREAD
				vthread_yield(server->thread);
				usleep(server->config->keepalive * 1000000);
#endif
			}

			if (ret == EREJECT)
			{
				server->run = 0;
			}
#ifdef VTHREAD
			vthread_yield(server->thread);
#endif
		}
		/// server->run may be changed from parent thread
		if (!server->run)
		{
			run--;
			server->ops->close(server);
		}
	}
	warn("server end");
	return ret;
}

static int _maxclients = DEFAULT_MAXCLIENTS;
http_server_t *httpserver_create(http_server_config_t *config)
{
	http_server_t *server;

	if (config->chunksize > 0)
		_buffer_chunksize(config->chunksize);

	server = vcalloc(1, sizeof(*server));
	if (server == NULL)
		return NULL;
	if (config)
		server->config = config;
	else
		server->config = &defaultconfig;
	_string_store(&server->name, httpserver_software, -1);
	struct utsname uts = {0}; /// uts->nodename should be hostname
	if (config->hostname)
		_string_store(&server->hostname, config->hostname, -1);
	else if (!uname(&uts))
		_string_store(&server->hostname, uts.nodename, -1);
	if (config->addr)
		_string_store(&server->addr, config->addr, -1);

	size_t length = snprintf(server->c_port, 5, "%.4d", config->port);
	_string_store(&server->s_port, server->c_port, length);
	if (config->service)
		_string_store(&server->service, config->service, -1);
	else if (config->hostname)
	{
		const char *dot = strchr(config->hostname, '.');
		const char *lastdot = strrchr(config->hostname, '.');
		if (dot && dot != lastdot)
		{
			_string_store(&server->service, config->hostname, dot - config->hostname);
		}
	}
	if (server->service.data == NULL)
		_string_store(&server->service, str_defaultsservice, sizeof(str_defaultsservice));
	server->ops = httpserver_ops;

	for (const http_message_method_t *method = default_methods; method; method = method->next)
	{
		httpserver_addmethod(server, method->key.data, method->key.length, method->properties);
	}
	vthread_init(server->config->maxclients);

	server->protocol_ops = tcpclient_ops;
	server->protocol = server;

	_maxclients += server->config->maxclients;
	if (nice(-4) <0)
		warn("not enought rights to change the process priority");
#ifdef USE_POLL
	server->poll_set =
#ifndef VTHREAD
		vcalloc(1 + server->config->maxclients, sizeof(*server->poll_set));
#else
		vcalloc(1, sizeof(*server->poll_set));
#endif
#endif

	if (server->ops->start(server))
	{
		free(server);
		return NULL;
	}
	warn("new server %p on port %d", server, server->config->port);

	return server;
}

http_server_t *httpserver_dup(http_server_t *server, http_server_config_t *config)
{
	http_server_t *vserver;

	vserver = vcalloc(1, sizeof(*vserver));
	if (vserver == NULL)
		return NULL;
	vserver->config = config;
	vserver->ops = server->ops;

	for (const http_message_method_t *method = default_methods; method; method = method->next)
	{
		httpserver_addmethod(vserver, method->key.data, method->key.length, method->properties);
	}

	vserver->protocol_ops = server->protocol_ops;
	vserver->protocol = server->protocol;

	return vserver;
}

void httpserver_addmethod(http_server_t *server, const char *key, size_t keylen, short properties)
{
	short id = -1;

	http_message_method_t *method;
	for (method = server->methods; method != NULL; method = method->next)
	{
		id = method->id;
		if (!_string_cmp(&method->key, key, -1))
		{
			break;
		}
	}
	if (method == NULL)
	{
		method = vcalloc(1, sizeof(*method));
		if (method == NULL)
			return;
		_string_store(&method->key, key, keylen);
		method->id = id + 1;
		method->next = server->methods;
		server->methods = method;

		if (server->methods_storage == NULL)
		{
			server->methods_storage = _buffer_create(str_methods, MAXCHUNKS_URI);
		}
		else
			_buffer_append(server->methods_storage, ",", 1);
		_buffer_append(server->methods_storage, method->key.data, method->key.length);
	}
	if (properties != method->properties)
	{
		method->properties |= properties;
	}
}

const httpclient_ops_t * httpserver_changeprotocol(http_server_t *server, const httpclient_ops_t *newops, void *config)
{
	const httpclient_ops_t *previous = server->protocol_ops;
	if (newops != NULL)
	{
		server->protocol_ops = newops;
		server->protocol = config;
	}
	return previous;
}

void httpserver_addmod(http_server_t *server, http_getctx_t modf, http_freectx_t unmodf, void *arg, const char *name)
{
	http_server_mod_t *mod = vcalloc(1, sizeof(*mod));
	if (mod == NULL)
		return;
	mod->func = modf;
	mod->freectx = unmodf;
	mod->arg = arg;
	mod->name = name;
	mod->next = server->mod;
	server->mod = mod;
}

void httpserver_addconnector(http_server_t *server,
						http_connector_t func, void *funcarg,
						int priority, const char *name)
{
	_httpconnector_add(&server->callbacks, func, funcarg, priority, name);
}

void httpserver_connect(http_server_t *server)
{
	struct rlimit rlim;
	getrlimit(RLIMIT_NOFILE, &rlim);
	/**
	 * need a file descriptors:
	 *  - for the server socket
	 *  - for each client socket
	 *  - for each file to send
	 *  - for stdin stdout stderr
	 *  - for websocket and other stream
	 */
	rlim.rlim_cur = _maxclients * 2 + 5 + MAXWEBSOCKETS;
	setrlimit(RLIMIT_NOFILE, &rlim);

#ifndef VTHREAD
	_httpserver_connect(server);
#else
	vthread_attr_t attr;

	vthread_create(&server->thread, &attr, (vthread_routine)_httpserver_run, (void *)server, sizeof(*server));
#endif
}

int httpserver_run(http_server_t *server)
{
#ifndef VTHREAD
	return _httpserver_run(server);
#else
	pause();
	return ECONTINUE;
#endif
}

int httpserver_reloadclient(http_server_t *server, http_client_t *client)
{
	httpclient_freeconnectors(client);
	httpclient_freemodules(client);
	_httpserver_setmod(server, client);
	for (http_connector_list_t *callback = server->callbacks; callback != NULL; callback = callback->next)
	{
		httpclient_addconnector(client, callback->func, callback->arg, callback->priority, callback->name);
	}
	return EREJECT;
}

void httpserver_disconnect(http_server_t *server)
{
	server->run = 0;
	server->ops->close(server);
	vthread_yield(server->thread);
}

void httpserver_destroy(http_server_t *server)
{
#ifdef VTHREAD
	if (server->thread)
	{
		vthread_join(server->thread, NULL);
		vthread_uninit(server->thread);
		server->thread = NULL;
	}
#endif
	http_connector_list_t *callback = server->callbacks;
	while (callback)
	{
		http_connector_list_t  *next = callback->next;
		vfree(callback);
		callback = next;
	}
	http_server_mod_t *mod = server->mod;
	while (mod)
	{
		http_server_mod_t *next = mod->next;
		vfree(mod);
		mod = next;
	}
	http_message_method_t *method = (http_message_method_t *)server->methods;
	while (method)
	{
		http_message_method_t *next = (http_message_method_t *) method->next;
		vfree(method);
		method = next;
	}
	if (server->methods_storage != NULL)
		_buffer_destroy(server->methods_storage);
	if (server->poll_set)
		vfree(server->poll_set);
	vfree(server);
}
/***********************************************************************/

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif
static const char default_value[8] = {0};
static char buffer[8];
const char *httpserver_INFO(http_server_t *server, const char *key)
{
	const char *value;
	httpserver_INFO2(server, key, &value);
	return value;
}

size_t httpserver_INFO2(http_server_t *server, const char *key, const char **value)
{
	size_t valuelen = 0;
	*value = default_value;

	if (!strcasecmp(key, "name") || !strcasecmp(key, "hostname"))
	{
		*value = server->hostname.data;
		valuelen = server->hostname.length;
	}
	else if (!strcasecmp(key, "domain"))
	{
		if (server->hostname.data)
			*value = strchr(server->hostname.data, '.');
		if (*value)
		{
			*value = *value + 1;
			valuelen = server->hostname.length - (*value - server->hostname.data);
		}
		else
			*value = default_value;
	}
	else if (!strcasecmp(key, "addr"))
	{
		*value = _string_get(&server->addr);
		valuelen = _string_length(&server->addr);
	}
	else if (!strcasecmp(key, "service"))
	{
		*value = server->service.data;
		valuelen = server->service.length;
	}
	else if (!strcasecmp(key, "software"))
	{
		*value = server->name.data;
		valuelen = server->name.length;
	}
	else if (!strcasecmp(key, "scheme"))
	{
		*value = server->protocol_ops->scheme;
		valuelen = -1;
	}
	else if (!strcasecmp(key, "protocol"))
	{
		valuelen = httpserver_version(server->config->version, value);
	}
	else if (!strcasecmp(key, "methods"))
	{
		*value = _buffer_get(server->methods_storage, 0);
		valuelen = _buffer_length(server->methods_storage);
	}
	else if (!strcasecmp(key, "secure"))
	{
		if (server->protocol_ops->type & HTTPCLIENT_TYPE_SECURE)
		{
			*value = str_true;
			valuelen = sizeof(str_true) - 1;
		}
		else
		{
			*value = str_false;
			valuelen = sizeof(str_false) - 1;
		}
	}
	else if (!strcasecmp(key, "port"))
	{
		*value = server->s_port.data;
		valuelen = server->s_port.length;
	}
	else if (!strcasecmp(key, "chunksize"))
	{
		valuelen = snprintf(buffer, 8, "%.7u", server->config->chunksize);
		*value = buffer;
	}
	return valuelen;
}

http_server_session_t *_httpserver_createsession(http_server_t *server, const http_client_t *client)
{
	http_server_session_t *session = NULL;
	session = vcalloc(1, sizeof(*session));
	if (session)
	{
		session->storage = _buffer_create(str_session, MAXCHUNKS_SESSION);
		/**
		 * the list should be managed with a lock.
		 * This is the only list directly used by several threads
		 * at the same time.
		 * But if concurent race arrives, the server may lost a session.
		 * The lost session is still able to be destroy, but not found.
		 * This case allows to have two clients with two different sessions,
		 * when they should be the same.
		 * Currently, the session info are build during authentication
		 * and may be built at each connection.
		 * A trouble may occure if a request create ID into the session,
		 * and another one would read this ID.
		 */
		session->ref = 1;
		session->next = server->sessions;
		server->sessions = session;
	}
	return session;
}

static void _httpserver_deletesession(http_server_t *server, http_server_session_t *session)
{
	http_server_session_t *it = server->sessions;
	if (it == session)
		server->sessions = session->next;
	else
	{
		while (it->next != NULL)
		{
			if (it->next == session)
			{
				it->next = session->next;
				break;
			}
			it = it->next;
		}
	}

	dbentry_destroy(session->dbfirst);
	session->dbfirst = NULL;
	_buffer_destroy(session->storage);
	free(session);
}

void _httpserver_dropsession(http_server_t *server, http_server_session_t *session)
{
	session->ref--;
	if (session->ref == 0)
		_httpserver_deletesession(server, session);
}

http_server_session_t *_httpserver_searchsession(const http_server_t *server, checksession_t cb, void *cbarg)
{
	for (http_server_session_t *it = server->sessions; it != NULL; it = it->next)
	{
		if (cb(cbarg, it) == ESUCCESS)
			return it;
	}
	return NULL;
}
