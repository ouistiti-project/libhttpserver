/*****************************************************************************
 * httpclient.c: Simple HTTP server
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

#define client_dbg(...)

static const char str_sockdata[] = "sockdata";
static const char str_header[] = "header";

static int _httpclient_thread(http_client_t *client);
static void _httpclient_destroy(http_client_t *client);
static int _httpclient_wait(http_client_t *client, int options);

http_client_t *httpclient_create(http_server_t *server, const httpclient_ops_t *fops, void *protocol)
{
	http_client_t *client = vcalloc(1, sizeof(*client));
	if (client == NULL)
	{
		err("client: not enough memory");
		return NULL;
	}
	client->server = server;
	client->ops = fops;
	client->protocol = protocol;
	_string_store(&client->scheme, fops->scheme, -1);

	client->client_send = client->ops->sendresp;
	client->client_recv = client->ops->recvreq;
	client->send_arg = client->opsctx;
	client->recv_arg = client->opsctx;
	client->sockdata = _buffer_create(str_sockdata, 1);
	if (client->sockdata == NULL)
	{
		err("client: not enough memory");
		_httpclient_destroy(client);
		return NULL;
	}
	client ->opsctx = client->ops->create(client->protocol, client);
	if (client ->opsctx == NULL)
	{
		err("client: protocol error");
		_httpclient_destroy(client);
		return NULL;
	}
#ifdef HTTPCLIENT_DUMPSOCKET
	char tmpname[] = "/tmp/ouistiticlient_XXXXXX.log";
	client->dumpfd = mkstemps(tmpname, 4);
	if (client->dumpfd > 0)
	{
		err("client: dump data");
		char address[INET_ADDRSTRLEN] = {0};
		inet_ntop(AF_INET, &client->addr, address,client);
		dprintf(client->dumpfd, "data from %s %s\n\n", address, server->c_port);
	}
	else
		err("client: dump data impossible %m");
#endif
	if (server)
	{
		for (http_server_mod_t *mod = server->mod; mod; mod = mod->next)
		{
			httpclient_addmodule(client, mod);
		}
		for (http_connector_list_t *callback = server->callbacks; callback != NULL; callback = callback->next)
		{
			httpclient_addconnector(client, callback->func, callback->arg, callback->priority, callback->name);
		}
#ifdef VTHREAD
		vthread_attr_t attr;
		httpclient_flag(client, 1, CLIENT_STOPPED);
		httpclient_flag(client, 0, CLIENT_STARTED);
		if (vthread_create(&client->thread, &attr, (vthread_routine)_httpclient_run, (void *)client, sizeof(*client)) != ESUCCESS)
		{
			httpclient_disconnect(client);
			httpclient_destroy(client);
			return NULL;
		}
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
#endif
	}
#ifdef HTTPCLIENT_FEATURES
	else
	{
		client->opsctx = client->ops->create(client->protocol, client);
		if (client->opsctx == NULL)
		{
			httpclient_destroy(client);
			return NULL;
		}
		client->send_arg = client->opsctx;
		client->recv_arg = client->opsctx;
	}
#endif

	return client;
}

static void _httpclient_destroy(http_client_t *client)
{
	if (client->opsctx != NULL)
	{
		client->ops->flush(client->opsctx);
		client->ops->disconnect(client->opsctx);
		client->ops->destroy(client->opsctx);
		client->opsctx = NULL;
	}

	httpclient_freemodules(client);
	httpclient_freeconnectors(client);
	if (client->session)
	{
		httpclient_dropsession(client);
	}
	if (client->sockdata)
		_buffer_destroy(client->sockdata);
	client->sockdata = NULL;
#ifdef HTTPCLIENT_DUMPSOCKET
	if (client->dumpfd > 0)
		close(client->dumpfd);
#endif
	http_message_t *request = client->request_queue;
	while (request)
	{
		http_message_t *next = request->next;
		_httpmessage_destroy(request);
		request = next;
	}
	client->request_queue = NULL;
	vfree(client);
}

void httpclient_destroy(http_client_t *client)
{
	_httpclient_destroy(client);
}

int httpclient_state(http_client_t *client, int newstate)
{
	if (newstate >= 0)
		client->state = newstate | (client->state & ~CLIENT_MACHINEMASK);
	return client->state;
}

void httpclient_flag(http_client_t *client, int remove, int new)
{
	if (!remove)
		client->state |= (new & ~CLIENT_MACHINEMASK);
	else
		client->state &= ~(new & ~CLIENT_MACHINEMASK);
}

void httpclient_freeconnectors(http_client_t *client)
{
	http_connector_list_t *callback = client->callbacks;
	while (callback != NULL)
	{
		http_connector_list_t *next = callback->next;
		free(callback);
		callback = next;
	}
	client->callbacks = NULL;
}

void httpclient_addconnector(http_client_t *client, http_connector_t func, void *funcarg, int priority, const char *name)
{
	_httpconnector_add(&client->callbacks, func, funcarg, priority, name);
}

int httpclient_addmodule(http_client_t *client, http_server_mod_t *mod)
{
	http_client_modctx_t *modctx = vcalloc(1, sizeof(*modctx));
	if (modctx == NULL)
		return EREJECT;
	if (mod->func)
	{
		modctx->ctx = mod->func(mod->arg, client, (struct sockaddr *)&client->addr, client->addr_size);
		if (modctx->ctx == NULL)
		{
			free(modctx);
			return EREJECT;
		}
	}
	modctx->freectx = mod->freectx;
	modctx->name = mod->name;
	modctx->next = client->modctx;
	client->modctx = modctx;
	return ESUCCESS;
}

void httpclient_freemodules(http_client_t *client)
{
	http_client_modctx_t *modctx = client->modctx;
	while (modctx)
	{
		http_client_modctx_t *next = modctx->next;
		if (modctx->freectx)
		{
			modctx->freectx(modctx->ctx);
		}
		free(modctx);
		modctx = next;
	}
	client->modctx = NULL;
}

void *httpclient_context(http_client_t *client)
{
	void *ctx = NULL;
	if (client->send_arg != NULL)
	{
		ctx = client->send_arg;
	}
	else if (client->recv_arg != NULL)
	{
		ctx = client->recv_arg;
	}
	else if (client->opsctx != NULL)
	{
		ctx = client->opsctx;
	}
	return ctx;
}

http_recv_t httpclient_addreceiver(http_client_t *client, http_recv_t func, void *arg)
{
	http_recv_t previous = client->client_recv;
	if (func)
	{
		client->client_recv = func;
		client->recv_arg = arg;
	}
	if (previous == NULL)
		previous = client->ops->recvreq;
	return previous;
}

http_send_t httpclient_addsender(http_client_t *client, http_send_t func, void *arg)
{
	http_send_t previous = client->client_send;
	if (func)
	{
		client->client_send = func;
		client->send_arg = arg;
	}
	if (previous == NULL)
		previous = client->ops->sendresp;
	return previous;
}

#ifdef HTTPCLIENT_FEATURES
static const httpclient_ops_t *_httpclient_ops;

void httpclient_appendops(const httpclient_ops_t *ops)
{
	if (_httpclient_ops == NULL)
		_httpclient_ops = ops;
	else
	{
		((httpclient_ops_t *)ops)->next = _httpclient_ops;
		_httpclient_ops = ops;
	}
}

const httpclient_ops_t *httpclient_ops()
{
	return _httpclient_ops;
}

int _httpclient_connect(http_client_t *client, const char *addr, int port)
{
	int ret = EREJECT;
	if (client && client->ops->connect)
	{
		ret = client->ops->connect(client->opsctx, addr, port);
	}
	return ret;
}

int httpclient_sendrequest(http_client_t *client, http_message_t *request, http_message_t *response)
{
	int size = 0;
	buffer_t *data = NULL;

	int ret = ESUCCESS;
	switch (request->state & GENERATE_MASK)
	{
	case GENERATE_INIT:
		request->client = client;

		ret = EINCOMPLETE;
		size = httpclient_wait(request->client, 1);
		if (size < 0)
			ret = EREJECT;
		request->state = GENERATE_RESULT;
	break;
	case GENERATE_RESULT:
	{
		/**
		 * send the request URI
		 */
		client->client_send(client->send_arg, request->method->key.data, request->method->key.length);
		client->client_send(client->send_arg, " /", 2);
		buffer_t *uri = request->uri;
		size = client->client_send(client->send_arg, _buffer_get(uri, 0), _buffer_length(uri));
		const char *version = NULL;
		size_t versionlen = httpserver_version(request->version, &version);
		if (version)
		{
			client->client_send(client->send_arg, " ", 1);
			client->client_send(client->send_arg, version, versionlen);
		}
		client->client_send(client->send_arg, "\r\n", 2);

		ret = EINCOMPLETE;
		request->state = GENERATE_HEADER;
	}
	break;
	case GENERATE_HEADER:
		/**
		 * send the header
		 */
		data = _httpmessage_buildheader(request);
		size = 0;
		while (_buffer_length(data) > size)
		{
			/**
			 * here, it is the call to the sendresp callback from the
			 * server configuration.
			 * see http_server_config_t and httpserver_create
			 */
			ret = _httpclient_wait(client, WAIT_SEND);
			if (ret == ESUCCESS)
			{
				int ret = client->client_send(client->send_arg, _buffer_get(data, size), _buffer_length(data) - size);
				if (ret < 0)
					break;
				size += ret;
			}
		}

		ret = EINCOMPLETE;
		request->state = GENERATE_SEPARATOR;
	break;
	case GENERATE_SEPARATOR:
		ret = _httpclient_wait(client, WAIT_SEND);
		if (ret == ESUCCESS)
		{
			size = client->client_send(client->send_arg, "\r\n", 2);
		}
		ret = EINCOMPLETE;
		request->state = GENERATE_CONTENT;
	break;
	case GENERATE_CONTENT:
		data = request->content;
		if (data == NULL)
			request->content_length = 0;
		size = 0;
		while (data && data->length > size)
		{
			/**
			 * here, it is the call to the sendresp callback from the
			 * server configuration.
			 * see http_server_config_t and httpserver_create
			 */
			ret = _httpclient_wait(client, WAIT_SEND);
			if (ret == EINCOMPLETE)
				continue;
			if (ret == ESUCCESS)
			{
				int ret = client->client_send(client->send_arg, _buffer_get(data, size), _buffer_length(data) - size);
				if (ret < 0)
					break;
				size += ret;
			}
		}
		if (!_httpmessage_contentempty(request, 1))
			request->content_length -= size;
		ret = EINCOMPLETE;
		if (_httpmessage_contentempty(request, 0) ||
			!_httpmessage_contentempty(request, 1))
		{
			request->state = GENERATE_END;

			response->client = client;
			response->state = PARSE_STATUS;
			response->content_length = -1;
			response->method = request->method;
		}
	break;
	case GENERATE_END:
		if (client->sockdata == NULL)
			client->sockdata = _buffer_create(str_sockdata, MAXCHUNKS_HEADER);

		data = client->sockdata;
		_buffer_reset(data, 0);

		ret = _httpclient_wait(request->client, WAIT_SEND);
		if (ret == ESUCCESS)
		{
			do {
				size = _buffer_fill(data, client->client_recv, client->recv_arg);
				sched_yield();
			} while (size == EINCOMPLETE);
		}
		if (size > 0)
		{
			data->length += size;
			data->data[data->length] = 0;
			dbg("client: response receive:\n%s", data->data);

			data->offset = data->data;
			ret = _httpmessage_parserequest(response, data);
			while ((ret == ECONTINUE) && (data->length - (data->offset - data->data) > 0))
			{
				_buffer_shrink(data);
				ret = _httpmessage_parserequest(response, data);
			}
		}
		if (size == EREJECT || _httpmessage_state(response, PARSE_END))
		{
			request->state = GENERATE_ERROR;
		}
	break;
	case GENERATE_ERROR:
		data = client->sockdata;
		_buffer_reset(data, 0);
		ret = ESUCCESS;
	break;
	}

	return ret;
}
#endif

int _httpclient_run(http_client_t *client)
{
	int ret = ECONTINUE;

	if (client->ops->start)
		ret = client->ops->start(client->opsctx);
	if (ret != ECONTINUE || client->opsctx == NULL)
	{
		httpclient_flag(client, 0, CLIENT_ERROR);
		httpclient_disconnect(client);
		return EREJECT;
	}
	client->send_arg = client->opsctx;
	client->recv_arg = client->opsctx;

	httpclient_flag(client, 1, CLIENT_STARTED);
	httpclient_flag(client, 0, CLIENT_RUNNING);

#ifdef VTHREAD
	dbg("client: %d %p thread start", vthread_self(client->thread), client);
	if (!vthread_sharedmemory(client->thread))
	{
		/*
		 * TODO : dispatch close and destroy from tcpserver.
		 */
		close(client->server->sock);
		client->server->sock = -1;
	}
	do
	{
		ret = _httpclient_thread(client);
	} while(ret == ECONTINUE || ret == EINCOMPLETE);
	/**
	 * When the connector manages it-self the socket,
	 * it possible to leave this thread without shutdown the socket.
	 * Be careful to not add action on the socket after this point
	 */
	httpclient_state(client, CLIENT_DEAD);
	dbg("client: %d %p thread exit", vthread_self(client->thread), client);
	if (!vthread_sharedmemory(client->thread))
		httpclient_destroy(client);
	else if (client->opsctx != NULL)
	{
		client->ops->flush(client->opsctx);
		client->ops->disconnect(client->opsctx);
		client->ops->destroy(client->opsctx);
		client->opsctx = NULL;
	}
#else
	do
	{
		ret = _httpclient_thread(client);
	}
	while (ret == EINCOMPLETE && client->request_queue == NULL);
	if (ret == ESUCCESS)
		httpclient_state(client, CLIENT_DEAD);
#endif
#ifdef DEBUG
	fflush(stderr);
#endif
	return ret;
}

int httpclient_socket(http_client_t *client)
{
	return client->sock;
}

http_server_t *httpclient_server(http_client_t *client)
{
	return client->server;
}

static int _httpclient_checkconnector(http_client_t *client, http_message_t *request, http_message_t *response, int priority)
{
	int ret = ESUCCESS;
	http_connector_list_t *first;
	first = client->callbacks;
	if (first == NULL)
		warn("client: no connector available");
	for (http_connector_list_t *callback = client->callbacks; callback != NULL; callback = callback->next)
	{
		if (response && callback->priority == CONNECTOR_COMPLETE)
		{
			if (response->complete == NULL)
				response->complete = callback;
			else
			{
				callback->nextcomplete = response->complete;
				response->complete = callback;
			}
		}
	}
	for (http_connector_list_t *callback = client->callbacks; callback != NULL; callback = callback->next)
	{
		if (callback->func)
		{
			if (priority > 0 && callback->priority != priority)
			{
				continue;
			}
			client_dbg("client %p connector \"%s\"", client, callback->name);
			ret = callback->func(callback->arg, request, response);
			if (ret != EREJECT)
			{
				if (ret == ESUCCESS)
				{
					httpclient_flag(client, 0, CLIENT_RESPONSEREADY);
				}
				request->connector = callback;
				break;
			}
			/**
			 * check if the connectors' list wasn't reloaded
			 */
			else if (first != client->callbacks)
			{
				first = client->callbacks;
				callback = first;
				continue;
			}
		}
	}
	return ret;
}

/**
 * @brief This function push a request when it is "ready".
 *
 * The request may be check by a connector when the header is fully received.
 * At this moment the request may be push into the list of request to response.
 * The request should contain the response into request->response.
 *
 * @param client the client connection to response.
 * @param request the request to response.
 */
static void _httpclient_pushrequest(http_client_t *client, http_message_t *request)
{
	http_message_t *iterator = client->request_queue;
	if (iterator == NULL)
	{
		client->request_queue = request;
	}
	else
	{
		while (iterator->next != NULL) iterator = iterator->next;
		iterator->next = request;
	}
}

static int _httpclient_error_connector(void *arg, http_message_t *request, http_message_t *response)
{
	if (request->response->result == RESULT_200)
		request->response->result = RESULT_404;
	_httpclient_checkconnector(arg, request, response, CONNECTOR_ERROR);
	_httpmessage_changestate(response, PARSE_END);
	return ESUCCESS;
}

static http_connector_list_t error_connector = {
	.func = _httpclient_error_connector,
	.arg = NULL,
	.next = NULL,
	.priority = CONNECTOR_ERROR,
	.name = "server",
};
/**
 * @brief This function receives data from the client connection and parse the request.
 *
 * The data is received chunck by chunck. If the connection is closing during the reception
 * the function will return EREJECT. If more data are mandatory the function return ECONTINUE.
 * The request will be created for the reception if it is not allready done. But
 * if the parsing if complete and the request is not complete (parsing error normaly or
 * the size of the content is unknown and the content is treated by the connector)
 * the request is reset.
 *
 * @param client the client connection to receive the data.
 * @param request the pointer to the request to fill.
 *
 * @return EINCOMPLETE : data is received and parsed, the function needs to be call again to read more data without waiting.
 * ECONTINUE: not enough data for parsing, need to wait more data.
 * ESUCCESS : the request is fully received and parsed. The request may be running.
 */
static int _httpclient_message(http_client_t *client, http_message_t *request)
{
	if (client->timeout == 0)
	{
		/**
		 * WAIT_ACCEPT does the first initialization
		 * otherwise the return is EREJECT
		 */
		int timer = WAIT_TIMER * 3;
		if (client->server->config->keepalive)
			timer = client->server->config->keepalive;
		client->timeout = timer * 100;
	}
	int ret = _httpmessage_parserequest(request, client->sockdata);

	if ((request->mode & HTTPMESSAGE_KEEPALIVE) &&
		!client->server->config->keepalive)
	{
		request->mode &= ~HTTPMESSAGE_KEEPALIVE;
	}

	if ((request->mode & HTTPMESSAGE_KEEPALIVE) &&
		(request->version > HTTP10))
	{
		dbg("client: set keep-alive");
		httpclient_flag(client, 0, CLIENT_KEEPALIVE);
	}
	return ret;
}

static int _httpclient_changeresponsestate(http_client_t *client, http_message_t *response, int ret)
{
	switch (ret)
	{
	case ESUCCESS:
	{
		httpclient_state(client, CLIENT_WAITING);
		if ((response->state & PARSE_MASK) < PARSE_POSTHEADER)
			_httpmessage_changestate(response, PARSE_POSTHEADER);
		if (!(response->state & GENERATE_MASK))
			_httpmessage_changestate(response, GENERATE_INIT);
		_httpmessage_changestate(response, PARSE_END);
		response->state &= ~PARSE_CONTINUE;
	}
	break;
	case ECONTINUE:
		if ((response->state & PARSE_MASK) < PARSE_POSTHEADER)
			_httpmessage_changestate(response, PARSE_POSTHEADER);
		if (!(response->state & GENERATE_MASK))
			_httpmessage_changestate(response, GENERATE_INIT);
		response->state |= PARSE_CONTINUE;
	break;
	case EINCOMPLETE:
		response->state |= PARSE_CONTINUE;
	break;
	case EREJECT:
	{
		_httpmessage_changestate(response, GENERATE_ERROR);
		response->state &= ~PARSE_CONTINUE;
		// The response is an error and it is ready to be sent
	}
	break;
	default:
		err("client: connector error");
	break;
	}
	return ret;
}

/**
 * @brief This function run the connector with the request.
 *
 * The request is "ready" (the header of the request is complete),
 * it is possible to check the list of connectors to find the good one.
 * If a connector returns EINCOMPLETE, the connector needs the content
 * before to send the response. For all other cases, the request is pushed
 * into the list of request to response.
 * The request may be not completed and to be pushed. In this case, this
 * function has not to do something.
 *
 * @param client the client connection which receives the request.
 * @param request the request to check.
 *
 * @return ESUCCESS : the request is pushed and the response is ready.
 * ECONTINUE : the request is pushed and the response needs to be build.
 * EINCOMPLETE :  the request is not ready to be pushed.
 */
static int _httpclient_request(http_client_t *client, http_message_t *request)
{
	int ret = ESUCCESS;

	/**
	 * The request is partially read.
	 * The connector can start to read the request when the header is ready.
	 * A problem is the size of the header. It is impossible to start
	 * the treatment before the end of the header, and it needs to
	 * store the header informations. It takes some place in  memory,
	 * depending of the server. It may be dangerous, a hacker can send
	 * a request with a very big header.
	 */
	if (request->response == NULL)
		request->response = _httpmessage_create(client, request);
	http_message_t *response = request->response;

	/**
	 * this condition is necessary for bad request parsing
	 */

	if ((response->state & PARSE_MASK) < PARSE_END)
	{
		if (request->connector == NULL)
		{
			ret = _httpclient_checkconnector(client, request, response, -1);
		}
		else if (response->state & PARSE_CONTINUE)
		{
			ret = _httpmessage_runconnector(request, response);
		}
		else
			ret = ECONTINUE;

		/**
		 * The request's content should be used by  "_httpclient_checkconnector"
		 * if it is required. After that the content is not stored and useless.
		 * The content is the "tempo" buffer, it is useless to free it.
		 **/
		request->content = NULL;
	}
	ret = _httpclient_changeresponsestate(client, response, ret);
	if (ret == EREJECT)
	{
		error_connector.arg = client;
		request->connector = &error_connector;
		ret = ESUCCESS;
	}
	else if ((request->mode & HTTPMESSAGE_LOCKED) ||
		(request->response->mode & HTTPMESSAGE_LOCKED))
	{
		httpclient_flag(client, 0, CLIENT_LOCKED);
	}
	return ret;
}

static int _httpclient_sendpart(http_client_t *client, buffer_t *buffer)
{
	int ret = ECONTINUE;
	if ((buffer != NULL) && (buffer->length > 0))
	{
		buffer->offset = buffer->data;
		int size = 0;

		while (buffer->length > 0)
		{
			size = client->client_send(client->send_arg, buffer->offset, buffer->length);
			if (size < 0)
				break;
			buffer->length -= size;
			buffer->offset += size;
		}
		if (size == EINCOMPLETE)
		{
			ret = EINCOMPLETE;
		}
		else if (size < 0)
		{
			err("client %p rest %lu send error %s", client, buffer->length, strerror(errno));
			/**
			 * error on sending the communication is broken and the thread must die
			 */
			ret = EREJECT;
		}
		else
			ret = ESUCCESS;
	}
	else
	{
		client_dbg("empty buffer to send");
	}
	return ret;
}

/**
 * @brief This function build and send the response of the request
 *
 * This function contains 2 parts: the first one runs the connector while
 * this one returns ECONTINUE (or EINCOMPLETE but it should not be the case).
 * The second part send the data on the client connection.
 *
 * @param client the client connection to response.
 * @param request the request of the response to send.
 *
 * @return ESUCCESS : the response is fully send.
 * ECONTINUE : the response is sending, and the function should be call again ASAP.
 * EINCOMPLETE : the connection is not ready to send data, and the function should be call again when it is ready.
 * EREJECT : the connection is closing during the sending.
 */
static int _httpclient_response_generate_error(http_client_t *client, http_message_t *request, http_message_t *response)
{
	int ret = ESUCCESS;
	if (response->version == HTTP09)
	{
		_httpmessage_changestate(response, GENERATE_CONTENT);
		ret = ECONTINUE;
	}
	else
	{
		if (response->header == NULL)
			response->header = _buffer_create(str_header, MAXCHUNKS_HEADER);
		buffer_t *buffer = response->header;
		_httpmessage_buildresponse(response,response->version, buffer);
		ret = EINCOMPLETE;
	}
	response->state &= ~PARSE_CONTINUE;
	return ret;
}
static int _httpclient_response_generate_init(http_client_t *client, http_message_t *request, http_message_t *response)
{
	int ret = ECONTINUE;
	if (response->version == HTTP09)
		_httpmessage_changestate(response, GENERATE_CONTENT);
	else
	{
		if (response->header == NULL)
			response->header = _buffer_create(str_header, MAXCHUNKS_HEADER);
		buffer_t *buffer = response->header;
		if ((response->state & PARSE_MASK) >= PARSE_POSTHEADER)
		{
			_httpmessage_buildresponse(response,response->version, buffer);
			ret = EINCOMPLETE;
		}
	}
	return ret;
}

static int _httpclient_response_generate_result(http_client_t *client, http_message_t *request, http_message_t *response)
{
	int ret = ESUCCESS;
	int sent;
	/**
	 * here, it is the call to the sendresp callback from the
	 * server configuration.
	 * see http_server_config_t and httpserver_create
	 */
	sent = _httpclient_sendpart(client, response->header);
	if (sent == EREJECT)
	{
		ret = EREJECT;
	}
	else if (sent == ESUCCESS)
	{
		/**
		 * for error the content must be set before the header
		 * generation to set the ContentLength
		 */
		if ((response->result >= 299) &&
			(response->content == NULL))
		{
			char value[_HTTPMESSAGE_RESULT_MAXLEN];
			size_t valuelen = _httpmessage_status(response, value, _HTTPMESSAGE_RESULT_MAXLEN);
			if (valuelen > 0)
				httpmessage_addcontent(response, "text/plain", value, valuelen);
			httpmessage_appendcontent(response, "\r\n", 2);
		}

		_httpmessage_changestate(response, GENERATE_HEADER);
		_buffer_destroy(response->header);
		response->header = NULL;
		int state = request->response->state;
		_httpmessage_buildheader(response);
		request->response->state = state;
		ret = EINCOMPLETE;
	}
	return ret;
}

static int _httpclient_response_generate_header(http_client_t *client, http_message_t *request, http_message_t *response)
{
	int ret = ESUCCESS;
	int sent;
	sent = _httpclient_sendpart(client, response->headers_storage);
	if (sent == ESUCCESS)
	{
		_httpmessage_changestate(response, GENERATE_SEPARATOR);
		ret = EINCOMPLETE;
	}
	else if (sent == EREJECT)
		ret = EREJECT;
	return ret;
}

static int _httpclient_response_generate_separator(http_client_t *client, http_message_t *request, http_message_t *response)
{
	int ret = ESUCCESS;
	int size;
	size = client->client_send(client->send_arg, "\r\n", 2);
	if (size < 0)
	{
		err("client %p SEPARATOR send error %s", client, strerror(errno));
		ret = EREJECT;
		return ret;
	}
	if (client->ops->flush != NULL)
		client->ops->flush(client->opsctx);
	/// Head method requires only the header
	if (request->method && request->method->id == MESSAGE_TYPE_HEAD)
	{
		if (response->content != NULL)
		{
			_buffer_destroy(response->content);
			response->content = NULL;
		}
		response->state &= ~PARSE_CONTINUE;
	}
	if (response->content != NULL)
	{
		int sent;
		size_t contentlength = _buffer_length(response->content);
		/**
		 * send the first part of the content.
		 * The next loop may append data into the content, but
		 * the first part has to be already sent
		 */
		if (_httpmessage_contentempty(response, 1))
			response->content_length -= contentlength;
		sent = _httpclient_sendpart(client, response->content);
		if (sent == EREJECT)
		{
			ret = EREJECT;
		}
		else
		{
			_buffer_reset(response->content, 0);
			_httpmessage_changestate(response, GENERATE_CONTENT);
			ret = ECONTINUE;
			response->state |= PARSE_CONTINUE;
		}
	}
	else if (response->state & PARSE_CONTINUE)
	{
		_httpmessage_changestate(response, GENERATE_CONTENT);
		ret = ECONTINUE;
	}
	else
	{
		_httpmessage_changestate(response, GENERATE_END);
		ret = ECONTINUE;
	}
	return ret;
}

static int _httpclient_response_generate_content(http_client_t *client, http_message_t *request, http_message_t *response)
{
	int ret = ESUCCESS;
	int sent = ESUCCESS;
	/**
	 * The module may send data by itself (mod_sendfile).
	 * In this case the content doesn't existe but the connector
	 * has to be called
	 */
	if (response->content != NULL && response->content->length > 0)
	{
		size_t contentlength = _buffer_length(response->content);
		if (!_httpmessage_contentempty(response, 1))
		{
			/**
			 * if for any raison the content_length is not the real
			 * size of the content the following condition must stop
			 * the request
			 */
			response->content_length -=
				(response->content_length < contentlength)?
				response->content_length : contentlength;
		}
		sent = _httpclient_sendpart(client, response->content);
		ret = ECONTINUE;
		if (_httpmessage_state(response, PARSE_END))
			_httpmessage_changestate(response, GENERATE_END);
		if (sent == EREJECT)
			ret = EREJECT;
#ifdef DEBUG
		static long long sent = 0;
		sent += contentlength;
		if (!_httpmessage_contentempty(response, 1) &&
			_httpmessage_state(response, PARSE_END))
		client_dbg("response send content %d %lld %lld", ret, response->content_length, sent);
#endif
		_buffer_reset(response->content, 0);
	}
	else
	{
		if (_httpmessage_state(response, PARSE_END) &&
			!(response->state & PARSE_CONTINUE))
		{
			_httpmessage_changestate(response, GENERATE_END);
		}
		ret = ECONTINUE;
	}
	return ret;
}

static int _httpclient_response_generate_end(http_client_t *client, http_message_t *request, http_message_t *response)
{
	if (response->content != NULL && response->content->length > 0)
	{
		_buffer_shrink(response->content);
	}
	const http_connector_list_t *callback = request->connector;
	const char *name = "server";
	if (callback)
		name = callback->name;
	const char *service = NULL;
	service = httpserver_INFO(client->server, "service");
	warn("client: %p response from connector \"%s\" service \"%s\" result %d", client, name, service, request->response->result);
	return ESUCCESS;
}

static int _httpclient_response(http_client_t *client, http_message_t *request)
{
	int ret = ESUCCESS;
	http_message_t *response = request->response;

	switch (response->state & GENERATE_MASK)
	{
		case 0:
		case GENERATE_ERROR:
			ret = _httpclient_response_generate_error(client, request, response);
		break;
		case GENERATE_INIT:
			ret = _httpclient_response_generate_init(client, request, response);
		break;
		case GENERATE_RESULT:
			ret = _httpclient_response_generate_result(client, request, response);
		break;
		case GENERATE_HEADER:
			ret = _httpclient_response_generate_header(client, request, response);
		break;
		case GENERATE_SEPARATOR:
			ret = _httpclient_response_generate_separator(client, request, response);
		break;
		case GENERATE_CONTENT:
			ret = _httpclient_response_generate_content(client, request, response);
		break;
		case GENERATE_END:
			ret = _httpclient_response_generate_end(client, request, response);
		break;
		default:
			err("client: bad state %X", response->state & GENERATE_MASK);
		break;
	}
	return ret;
}

/**
 * @brief This function waits data on the client socket.
 *
 * @param client the client connection to trigg
 * @param options the bitsmask with WAIT_ACCEPT and WAIT_SEND
 *
 * @return ESUCCESS : data are available.
 * EINCOMPLETE : data is not available, wait again.
 * EREJECT : connection must be close.
 */
static int _httpclient_wait(http_client_t *client, int options)
{
	int ret;
	ret = client->ops->wait(client->opsctx, options);
	return ret;
}

int httpclient_wait(http_client_t *client, int options)
{
	int ret;
	ret = _httpclient_wait(client, options);
	if (ret == ESUCCESS)
		ret = client->sock;
	return ret;
}

int _httpclient_geterror(http_client_t *client)
{
	if (client->request == NULL)
		return EREJECT;
	/**
	 * The request contains an syntax error and must be rejected
	 */
	if (client->request->response == NULL)
		client->request->response = _httpmessage_create(client, client->request);

	error_connector.arg = client;
	client->request->connector = &error_connector;
	_httpmessage_changestate(client->request->response, PARSE_CONTENT);
	client->request->response->state |= PARSE_CONTINUE;
	_httpmessage_changestate(client->request->response, GENERATE_ERROR);
	/**
	 * The format of the request is bad. It may be an attack.
	 */
	warn("client: bad request");
	_httpmessage_changestate(client->request, PARSE_END);
	client->request = NULL;
	return ESUCCESS;
}

int _httpclient_isalive(http_client_t *client)
{
	if ((client->state & CLIENT_MACHINEMASK) == CLIENT_DEAD)
		return EREJECT;
#ifdef VTHREAD
	vthread_yield(client->thread);
	if (!vthread_exist(client->thread))
		return EREJECT;
#endif
	return ESUCCESS;
}

static int _httpclient_thread_statemachine(http_client_t *client)
{
	int ret = ECONTINUE;
	int wait_option = 0;
	if (client->state & CLIENT_STOPPED)
		httpclient_state(client, CLIENT_EXIT);

	switch (client->state & CLIENT_MACHINEMASK)
	{
		case CLIENT_NEW:
			wait_option = WAIT_ACCEPT;
		case CLIENT_WAITING:
		{
			ret = ESUCCESS;
			if (_buffer_empty(client->sockdata))
				ret = _httpclient_wait(client, wait_option);
			/// timeout on socket
			if (ret == EREJECT && errno == EAGAIN)
			{
				err("client: %p timeout", client);
				httpclient_flag(client, 0, CLIENT_STOPPED);
#if 0
				ret = ESUCCESS;
#endif
			}
		}
		break;
		case CLIENT_READING:
		{
			if (!_buffer_full(client->sockdata))
				ret = client->ops->status(client->opsctx);
		}
		break;
		case CLIENT_SENDING:
		{
			_httpclient_wait(client, WAIT_SEND);
			if (_buffer_empty(client->sockdata))
				ret = client->ops->status(client->opsctx);
		}
		break;
		case CLIENT_EXIT:
		{
			/**
			 * flush the output socket
			 */
			if (client->ops->flush != NULL)
				client->ops->flush(client->opsctx);

			/**
			 * the modules need to be free before any
			 * socket closing.
			 * This part may not be into destroy function, because this
			 * one is called by the vthread parent after that the client
			 * died.
			 */
			httpclient_freemodules(client);
			if (!(client->state & CLIENT_LOCKED))
				client->ops->disconnect(client->opsctx);

			httpclient_flag(client, 0, CLIENT_STOPPED);
			ret = ESUCCESS;
		}
		default:
		break;
	}
	return ret;
}

static int _httpclient_thread_receive(http_client_t *client)
{

	int size;
	if (client->state & CLIENT_STOPPED)
		return ESUCCESS;

	/**
	 * here, it is the call to the recvreq callback from the
	 * server configuration.
	 * see http_server_config_t and httpserver_create
	 */
	_buffer_shrink(client->sockdata);
	_buffer_reset(client->sockdata, _buffer_length(client->sockdata));
	size = _buffer_fill(client->sockdata, client->client_recv, client->recv_arg);
	if (size == 0 || size == EREJECT)
	{
		/**
		 * error on the connection
		 */
		_httpclient_geterror(client);
		httpclient_state(client, CLIENT_EXIT);
		httpclient_flag(client, 0, CLIENT_ERROR);
		return ECONTINUE;
	}
	else if (size == EINCOMPLETE)
	{
		httpclient_state(client, CLIENT_WAITING);
	}
	else
	{
		/**
		 * the buffer must always be read from the beginning
		 */
		client->sockdata->offset = client->sockdata->data;

		httpclient_state(client, CLIENT_READING);
#ifdef HTTPCLIENT_DUMPSOCKET
		if (client->dumpfd > 0)
			write(client->dumpfd, client->sockdata->data, size);
#endif
	}
	return EINCOMPLETE;
}

static void _httpclient_thread_fillrequest(http_client_t *client)
{
	/**
	 * the message must be create in all cases
	 * the sockdata may contain a new message
	 */
	if (client->request == NULL)
	{
		client->request = _httpmessage_create(client, NULL);
		_httpclient_pushrequest(client, client->request);
	}

	/**
	 * Some data availables
	 */
	int ret = _httpclient_message(client, client->request);
	switch (ret)
	{
	case ECONTINUE:
	{
		/**
		 * The request is ready to be manipulate by the connectors
		 */
		httpclient_state(client, CLIENT_WAITING);
	}
	break;
	case EINCOMPLETE:
	{
#if 0
		/**
		 * this version may be faster, but it is impossible to trigg message
		 * with a smaller Content than the Content-Length
		 */
		if (_buffer_empty(client->sockdata))
		{
			/**
			 * The request is not ready and need more data
			 */
			httpclient_state(client, CLIENT_WAITING);
		}
		else
		{
			httpclient_state(client, CLIENT_READING);
		}
#else
		httpclient_state(client, CLIENT_WAITING);
#endif
	}
	break;
	case EREJECT:
	{
		_httpclient_geterror(client);

		httpclient_state(client, CLIENT_READING);
		httpclient_flag(client, 0, CLIENT_ERROR);
		_buffer_reset(client->sockdata, 0);
	}
	break;
	case ESUCCESS:
	default:
	{
		/**
		 * postheader already shrink the buffer.
		 * for message without content this shrink is dangerous.
		 */
		if (client->request->content_length != 0)
		{
			_buffer_shrink(client->sockdata);
		}
		client->request = NULL;
		httpclient_state(client, CLIENT_SENDING);
	}
	}
}

static void _httpclient_thread_parserequest(http_client_t *client, http_message_t *request)
{
	int ret = ESUCCESS;
	if (!request->response)
	{
		/**
		 * connector first call
		 * response doesnt exist
		 */
		ret = _httpclient_request(client, request);
	}
	else if ((request->response->state & PARSE_MASK) < PARSE_END)
	{
		/**
		 * connector continue to chech data
		 */
		ret = _httpclient_request(client, request);
	}
	else
	{
		/**
		 * connector has already responded ESUCCESS
		 * it has not to be call again.
		 */
	}
	if (ret == EREJECT)
	{
		httpclient_state(client, CLIENT_EXIT);
	}
	else if (ret == EINCOMPLETE)
	{
		/**
		 * The request is ready and all headers are parsed.
		 * The connector is not ready and must be call again with more data
		 */
	}
	else if (ret == ESUCCESS)
	{
		request->response->state &= ~PARSE_CONTINUE;
	}
}

static int _httpclient_thread_generateresponse(http_client_t *client, http_message_t *request)
{
	int ret = ECONTINUE;
	http_message_t *response = request->response;

	if ((response->state & GENERATE_MASK) > 0)
	{
		int res_ret = EINCOMPLETE;
		do
		{
			res_ret = _httpclient_response(client, request);
		} while (res_ret == EINCOMPLETE);

		if (res_ret == ESUCCESS)
		{
			if (_httpmessage_contentempty(response, 1))
			{
				dbg("client: disable keep alive (Content-Length is not set)");
				httpclient_flag(client, 1, CLIENT_KEEPALIVE);
			}

			if ((request->state & PARSE_MASK) < PARSE_END)
			{
				client_dbg("client: incomplete");
				httpclient_state(client, CLIENT_EXIT);
				ret = EINCOMPLETE;
			}
			else if (client->state & CLIENT_ERROR)
			{
				client_dbg("client: error");
				httpclient_state(client, CLIENT_EXIT);
			}
			else if (client->state & CLIENT_LOCKED)
			{
				client_dbg("client: locked");
				httpclient_state(client, CLIENT_EXIT);
			}
			else if ((client->state & CLIENT_KEEPALIVE) &&
					(httpmessage_result(response, -1) < 400))
			{
				client_dbg("client: keep alive");
				httpclient_state(client, CLIENT_READING);
			}
			else
			{
				client_dbg("client: exit");
				httpclient_state(client, CLIENT_EXIT);
				ret = EINCOMPLETE;
			}
			/**
			 * client->request is not null if the reception is not complete.
			 * In this case the client keeps the request until the connection
			 * is closed
			 */
			dbg("client: response complete");
			client->request_queue = request->next;
			_httpmessage_destroy(request);
		}
		else if (res_ret == EREJECT)
		{
			err("client should exit");
			httpclient_state(client, CLIENT_EXIT);
		}
		else
			httpclient_state(client, CLIENT_SENDING);
	}
	return ret;
}

/**
 * @brief This function is the manager of the client's loop.
 *
 * This function does:
 *  - wait data if it is useful.
 *  - read and parse data to build a request.
 *  - check if the receiving request could be treat by a connector.
 *  - treat the list of requests already ready to response.
 *
 * @param client the client connection.
 *
 * @return ESUCCESS : The client is closed and the loop may stop.
 * ECONTINUE : The main loop must continue to run.
 */
static int _httpclient_thread(http_client_t *client)
{
#ifdef DEBUG
	struct timespec spec;
	clock_gettime(CLOCK_MONOTONIC, &spec);
	//dbg("\tclient %p state %X at %d:%d", client, client->state, spec.tv_sec, spec.tv_nsec);
#endif
	int ret = ECONTINUE;
	ret = _httpclient_thread_statemachine(client);

	if ((ret == ESUCCESS) && (client->state & CLIENT_STOPPED))
	{
		return ret;
	}
	else if ((ret == ESUCCESS) && !(client->state & CLIENT_LOCKED))
	{
		int ret = _httpclient_thread_receive(client);
		if (ret != EINCOMPLETE)
		{
			return ret;
		}
	}
	else if (ret == EREJECT)
	{
		err("client: message in error");
		_httpclient_geterror(client);
		httpclient_state(client, CLIENT_EXIT);
		httpclient_flag(client, 0, CLIENT_ERROR);
		return ECONTINUE;
	}
	else if (ret == EINCOMPLETE)
	{
		httpclient_state(client, CLIENT_WAITING);
	}

	/**
	 * manage a request with the socket data
	 */
	if (!_buffer_empty(client->sockdata))
	{
		_httpclient_thread_fillrequest(client);
	}

	/**
	 * manage the request occuring or already received
	 * the socket data may fill a new other request at the same time
	 */
	ret = ECONTINUE;
	http_message_t *request = client->request_queue;
	if (request != NULL &&
		((request->state & PARSE_MASK) > PARSE_PRECONTENT))
	{
		_httpclient_thread_parserequest(client, request);
		ret = _httpclient_thread_generateresponse(client, request);
	}
	return ret;
}

void httpclient_shutdown(http_client_t *client)
{
	client->ops->disconnect(client->opsctx);
	httpclient_state(client, CLIENT_EXIT);
}

void httpclient_flush(http_client_t *client)
{
	if (client->ops->flush)
		client->ops->flush(client->opsctx);
}

#define SESSION_SEPARATOR " "
static int _httpclient_checksession(void * arg, http_server_session_t*session)
{
	const char *token = (const char *)arg;
	dbentry_t *entry = dbentry_get(session->dbfirst, "token");
	if (entry && !strncmp(token, entry->storage->data + entry->value.offset, entry->value.length))
	{
		return ESUCCESS;
	}
	return EREJECT;
}

int httpclient_setsession(http_client_t *client, const char *token, size_t tokenlen)
{
	if (client->session)
		return EREJECT;
	client->session = _httpserver_searchsession(client->server, _httpclient_checksession, (void *)token);
	if (client->session == NULL)
		client->session = _httpserver_createsession(client->server, client);
	else
		client->session->ref++;

	_buffer_append(client->session->storage, STRING_REF("token="));
	_buffer_append(client->session->storage, token, tokenlen);
	_buffer_append(client->session->storage, SESSION_SEPARATOR, 1);

	return _buffer_filldb(client->session->storage, &client->session->dbfirst, '=', SESSION_SEPARATOR[0]);
}

void httpclient_dropsession(http_client_t *client)
{
	if (!client->session)
		return;

	_httpserver_dropsession(client->server, client->session);
	client->session = NULL;
}

static dbentry_t * _httpclient_sessioninfo(http_client_t *client, const char *key)
{
	dbentry_t *sessioninfo = NULL;
	if (client == NULL)
		return NULL;
	if (client->session == NULL)
		return NULL;
	if (key == NULL)
		return NULL;

	sessioninfo = dbentry_get(client->session->dbfirst, key);

	return sessioninfo;
}

const void *httpclient_session(http_client_t *client, const char *key, size_t keylen, const void *value, size_t size)
{
	if (client->session == NULL)
		return NULL;
	dbentry_t *entry = dbentry_get(client->session->dbfirst, key);
	if (value != NULL)
	{
		if (size == 0)
			return NULL;
		if (entry != NULL)
		{
#if 0
			_buffer_deletedb(client->session->storage, entry, 0);
#else
			client->session->storage->data[entry->key.offset] = SESSION_SEPARATOR[0];
#endif
		}
		int keyof = _buffer_append(client->session->storage, key, keylen);
		_buffer_append(client->session->storage, "=", 1);
		int valueof = _buffer_append(client->session->storage, value, size);
		_buffer_append(client->session->storage, SESSION_SEPARATOR, 1);
#if 0
		dbentry_destroy(client->session->dbfirst);
		client->session->dbfirst = NULL;
		_buffer_filldb(client->session->storage, &client->session->dbfirst, '=', SESSION_SEPARATOR[0]);
#else
		key = _buffer_get(client->session->storage, keyof);
		value = _buffer_get(client->session->storage, valueof);
		_buffer_dbentry(client->session->storage, &client->session->dbfirst, key, keylen, value, client->session->storage->length - 1);
#endif
		entry = dbentry_get(client->session->dbfirst, key);
	}
	else if (entry == NULL)
	{
		return NULL;
	}
	return client->session->storage->data + entry->value.offset;
}

const void *httpclient_appendsession(http_client_t *client, const char *key, const void *value, size_t size)
{
	dbentry_t *entry = _httpclient_sessioninfo(client, key);
	if (entry == NULL)
		return NULL;
	void *valueend = client->session->storage->data + entry->value.offset + entry->value.length;
	if (valueend + sizeof(SESSION_SEPARATOR[0]) + 1 != client->session->storage->offset)
	{
		// Lost the current value of the entry. May be the buffer should be shrink...
		_buffer_append(client->session->storage, client->session->storage->data + entry->value.offset, entry->value.length);
		entry->value.offset = client->session->storage->offset - entry->value.length - client->session->storage->data;
		_buffer_append(client->session->storage, SESSION_SEPARATOR, 1);
	}
	_buffer_pop(client->session->storage, 1);
	_buffer_append(client->session->storage, value, size);
	_buffer_append(client->session->storage, SESSION_SEPARATOR, 1);
	return client->session->storage->data + entry->value.offset;
}

