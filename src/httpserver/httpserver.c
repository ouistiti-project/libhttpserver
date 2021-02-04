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

#ifdef USE_STDARG
#include <stdarg.h>
#endif

#ifdef USE_POLL
#include <poll.h>
#else
#include <sys/select.h>
#endif

#include <netdb.h>

#include "valloc.h"
#include "vthread.h"
#include "log.h"
#include "httpserver.h"
#include "_httpserver.h"
#include "_httpmessage.h"
#include "_buffer.h"
#include "dbentry.h"

#define buffer_dbg(...)
#define message_dbg(...)
#define client_dbg(...)
#define server_dbg(...)

extern httpserver_ops_t *httpserver_ops;

struct http_server_mod_s
{
	void *arg;
	const char *name;
	http_getctx_t func;
	http_freectx_t freectx;
	http_server_mod_t *next;
};

struct http_client_modctx_s
{
	void *ctx;
	const char *name;
	http_freectx_t freectx;
	http_client_modctx_t *next;
};

static int _httpclient_run(http_client_t *client);
static void _http_addconnector(http_connector_list_t **first,
						http_connector_t func, void *funcarg,
						int priority, const char *name);

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
const char str_defaultscheme[] = "http";
#endif
const char str_true[] = "true";
const char str_false[] = "false";

static char _httpserver_software[] = "libhttpserver";
char *httpserver_software = _httpserver_software;
/********************************************************************/
/***********************************************************************
 * http_client
 */
static void _httpclient_destroy(http_client_t *client);

http_client_t *httpclient_create(http_server_t *server, const httpclient_ops_t *fops, void *protocol)
{
	http_client_t *client = vcalloc(1, sizeof(*client));
	if (client == NULL)
		return NULL;
	client->server = server;

	if (server)
	{
		http_connector_list_t *callback = server->callbacks;
		while (callback != NULL)
		{
			httpclient_addconnector(client, callback->func, callback->arg, callback->priority, callback->name);
			callback = callback->next;
		}
	}
	client->ops = fops;
	client->opsctx = client->ops->create(protocol, client);
	client->client_send = client->ops->sendresp;
	client->client_recv = client->ops->recvreq;
	client->send_arg = client->opsctx;
	client->recv_arg = client->opsctx;
	if (client->opsctx != NULL)
	{
		client->sockdata = _buffer_create(1);
	}
	if (client->sockdata == NULL)
	{
		_httpclient_destroy(client);
		client = NULL;
	}

	return client;
}

static void _httpclient_destroy(http_client_t *client)
{
	if (client->opsctx != NULL)
		client->ops->destroy(client->opsctx);

	client->modctx = NULL;
	http_connector_list_t *callback = client->callbacks;
	while (callback != NULL)
	{
		http_connector_list_t *next = callback->next;
		free(callback);
		callback = next;
	}
	if (client->session)
	{
		dbentry_t *db = client->session->dbfirst;
		while (db)
		{
			dbentry_t *next = db->next;
			free(db);
			db = next;
		}
		vfree(client->session->storage);
		vfree(client->session);
	}
	if (client->sockdata)
		_buffer_destroy(client->sockdata);
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

void httpclient_state(http_client_t *client, int new)
{
	client->state = new | (client->state & ~CLIENT_MACHINEMASK);
}

void httpclient_flag(http_client_t *client, int remove, int new)
{
	if (!remove)
		client->state |= (new & ~CLIENT_MACHINEMASK);
	else
		client->state &= ~(new & ~CLIENT_MACHINEMASK);
}

void httpclient_addconnector(http_client_t *client, http_connector_t func, void *funcarg, int priority, const char *name)
{
	_http_addconnector(&client->callbacks, func, funcarg, priority, name);
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
		const char *method = httpmessage_REQUEST(request, "method");
		client->client_send(client->send_arg, method, strlen(method));
		client->client_send(client->send_arg, " /", 2);
		const char *uri = httpmessage_REQUEST(request, "uri");
		size = client->client_send(client->send_arg, uri, strlen(uri));
		const char *version = httpmessage_REQUEST(request, "version");
		if (version)
		{
			client->client_send(client->send_arg, " ", 1);
			client->client_send(client->send_arg, version, strlen(version));
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
		while (data->length > 0)
		{
			/**
			 * here, it is the call to the sendresp callback from the
			 * server configuration.
			 * see http_server_config_t and httpserver_create
			 */
			size = httpclient_wait(client, 1);
			if (size > 0)
			{
				size = client->client_send(client->send_arg, data->offset, data->length);
			}
			if (size == EINCOMPLETE)
				continue;
			if (size < 0)
				break;
			data->offset += size;
			data->length -= size;
		}

		ret = EINCOMPLETE;
		request->state = GENERATE_SEPARATOR;
	break;
	case GENERATE_SEPARATOR:
		size = client->client_send(client->send_arg, "\r\n", 2);
		ret = EINCOMPLETE;
		request->state = GENERATE_CONTENT;
	break;
	case GENERATE_CONTENT:
		data = request->content;
		data->offset = data->data;
		while (data->length > 0)
		{
			/**
			 * here, it is the call to the sendresp callback from the
			 * server configuration.
			 * see http_server_config_t and httpserver_create
			 */
			size = httpclient_wait(client, 1);
			if (size > 0)
			{
				size = client->client_send(client->send_arg, data->offset, data->length);
			}
			if (size == EINCOMPLETE)
				continue;
			if (size < 0)
				break;
			data->offset += size;
			data->length -= size;
			if (!_httpmessage_contentempty(request, 1))
				request->content_length -= size;
		}
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
			client->sockdata = _buffer_create(MAXCHUNKS_HEADER);

		data = client->sockdata;
		_buffer_reset(data);
		*(data->offset) = '\0';

		size = httpclient_wait(request->client, 1);
		if (size < 0)
			ret = EREJECT;
		if (!_httpmessage_contentempty(response, 1) && size > 0)
		{
			size = client->client_recv(client->recv_arg, data->offset, data->size - data->length);

		}
		if (size > 0)
		{
			data->length += size;
			data->data[data->length] = 0;

			data->offset = data->data;
			ret = _httpmessage_parserequest(response, data);
			while ((ret == ECONTINUE) && (data->length - (data->offset - data->data) > 0))
			{
				_buffer_shrink(data, 1);
				ret = _httpmessage_parserequest(response, data);
			}
		}
		if (_httpmessage_state(response, PARSE_END))
		{
			request->state = GENERATE_ERROR;
		}
	break;
	case GENERATE_ERROR:
		data = client->sockdata;
		_buffer_reset(data);
		*(data->offset) = '\0';
		ret = ESUCCESS;
	break;
	}

	return ret;
}
#endif

#ifdef VTHREAD
static int _httpclient_connect(http_client_t *client)
{
	int ret;
	httpclient_flag(client, 1, CLIENT_STARTED);
	httpclient_flag(client, 0, CLIENT_RUNNING);
#ifndef SHARED_SOCKET
	/*
	 * TODO : dispatch close and destroy from tcpserver.
	 */
	close(client->server->sock);
	client->server->sock = -1;
#endif
	do
	{
		ret = _httpclient_run(client);
	} while(ret == ECONTINUE || ret == EINCOMPLETE);
	/**
	 * When the connector manages it-self the socket,
	 * it possible to leave this thread without shutdown the socket.
	 * Be careful to not add action on the socket after this point
	 */
	client->state = CLIENT_DEAD | (client->state & ~CLIENT_MACHINEMASK);
	warn("client %p thread exit", client);
	httpclient_destroy(client);
#ifdef DEBUG
	fflush(stderr);
#endif
	return 0;
}
#endif

int httpclient_socket(http_client_t *client)
{
	return client->sock;
}

http_server_t *httpclient_server(http_client_t *client)
{
	return client->server;
}

static int _httpclient_checkconnector(http_client_t *client, http_message_t *request, http_message_t *response)
{
	int ret = ESUCCESS;
	http_connector_list_t *iterator;
	iterator = client->callbacks;
	while (iterator != NULL)
	{
		if (iterator->func)
		{
			client_dbg("client %p connector \"%s\"", client, iterator->name);
			ret = iterator->func(iterator->arg, request, response);
			if (ret != EREJECT)
			{
				if (ret == ESUCCESS)
				{
					httpclient_flag(client, 0, CLIENT_RESPONSEREADY);
				}
				request->connector = iterator;
				break;
			}
		}
		iterator = iterator->next;
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
	dbg("error connector");
	return ESUCCESS;
}

static http_connector_list_t error_connector = {
	.func = _httpclient_error_connector,
	.arg = NULL,
	.next = NULL,
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
	/**
	 * WAIT_ACCEPT does the first initialization
	 * otherwise the return is EREJECT
	 */
	int timer = WAIT_TIMER * 3;
	if (client->server->config->keepalive)
		timer = client->server->config->keepalive;
	client->timeout = timer * 100;

	int ret = _httpmessage_parserequest(request, client->sockdata);

	if ((request->mode & HTTPMESSAGE_KEEPALIVE) &&
		(request->version > HTTP10))
	{
		warn("request: set keep-alive");
		httpclient_flag(client, 0, CLIENT_KEEPALIVE);
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
			ret = _httpclient_checkconnector(client, request, response);
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
		switch (ret)
		{
		case ESUCCESS:
		{
			client->state = CLIENT_WAITING | (client->state & ~CLIENT_MACHINEMASK);
			if ((response->state & PARSE_MASK) < PARSE_POSTHEADER)
				_httpmessage_changestate(response, PARSE_POSTHEADER);
			if (!(response->state & GENERATE_MASK))
				_httpmessage_changestate(response, GENERATE_INIT);
			_httpmessage_changestate(response, PARSE_END);
			response->state &= ~PARSE_CONTINUE;
			if (request->mode & HTTPMESSAGE_LOCKED)
			{
				client->state |= CLIENT_LOCKED;
			}
		}
		break;
		case ECONTINUE:
			if ((response->state & PARSE_MASK) < PARSE_POSTHEADER)
				_httpmessage_changestate(response, PARSE_POSTHEADER);
			if (!(response->state & GENERATE_MASK))
				_httpmessage_changestate(response, GENERATE_INIT);
			response->state |= PARSE_CONTINUE;
			if ((request->mode & HTTPMESSAGE_LOCKED) ||
				(request->response->mode & HTTPMESSAGE_LOCKED))
			{
				client->state |= CLIENT_LOCKED;
			}
		break;
		case EINCOMPLETE:
			response->state |= PARSE_CONTINUE;
		break;
		case EREJECT:
		{
			request->connector = &error_connector;
			_httpmessage_changestate(response, GENERATE_ERROR);
			request->response->state &= ~PARSE_CONTINUE;
			// The response is an error and it is ready to be sent
			ret = ESUCCESS;
		}
		break;
		default:
			err("client: connector error");
		break;
		}
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
			err("client %p rest %d send error %s", client, buffer->length, strerror(errno));
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
static int _httpclient_response(http_client_t *client, http_message_t *request)
{
	int ret = ESUCCESS;
	http_message_t *response = request->response;

	switch (response->state & GENERATE_MASK)
	{
		case 0:
		case GENERATE_ERROR:
		{
			if (response->version == HTTP09)
			{
				_httpmessage_changestate(response, GENERATE_CONTENT);
				ret = ECONTINUE;
			}
			else
			{
				if (response->header == NULL)
					response->header = _buffer_create(MAXCHUNKS_HEADER);
				buffer_t *buffer = response->header;
				_httpmessage_buildresponse(response,response->version, buffer);
				_httpmessage_changestate(response, GENERATE_RESULT);
				ret = EINCOMPLETE;
			}
			response->state &= ~PARSE_CONTINUE;
			client->state |= CLIENT_LOCKED;
		}
		break;
		case GENERATE_INIT:
		{
			ret = ECONTINUE;
			if (response->version == HTTP09)
				_httpmessage_changestate(response, GENERATE_CONTENT);
			else
			{
				if (response->header == NULL)
					response->header = _buffer_create(MAXCHUNKS_HEADER);
				buffer_t *buffer = response->header;
				if ((response->state & PARSE_MASK) >= PARSE_POSTHEADER)
				{
					_httpmessage_changestate(response, GENERATE_RESULT);
					_httpmessage_buildresponse(response,response->version, buffer);
					ret = EINCOMPLETE;
				}
			}
		}
		break;
		case GENERATE_RESULT:
		{
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
					const char *value = _httpmessage_status(response);
					httpmessage_addcontent(response, "text/plain", value, strlen(value));
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
		}
		break;
		case GENERATE_HEADER:
		{
			int sent;
			sent = _httpclient_sendpart(client, response->headers_storage);
			if (sent == ESUCCESS)
			{
				_httpmessage_changestate(response, GENERATE_SEPARATOR);
				ret = EINCOMPLETE;
			}
			else if (sent == EREJECT)
				ret = EREJECT;
		}
		break;
		case GENERATE_SEPARATOR:
		{
			int size;
			size = client->client_send(client->send_arg, "\r\n", 2);
			if (size < 0)
			{
				err("client %p SEPARATOR send error %s", client, strerror(errno));
				ret = EREJECT;
				break;
			}
			client->ops->flush(client->opsctx);
			if (request->method && request->method->id == MESSAGE_TYPE_HEAD)
			{
				_httpmessage_changestate(response, GENERATE_END);
				ret = ECONTINUE;
			}
			else if (response->content != NULL)
			{
				int sent;
				/**
				 * send the first part of the content.
				 * The next loop may append data into the content, but
				 * the first part has to be already sent
				 */
				if (!_httpmessage_contentempty(response, 1))
					response->content_length -= response->content->length;
				sent = _httpclient_sendpart(client, response->content);
				if (sent == EREJECT)
				{
					ret = EREJECT;
				}
				else
				{
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
		}
		break;
		case GENERATE_CONTENT:
		{
			int sent = ESUCCESS;
			/**
			 * The module may send data by itself (mod_sendfile).
			 * In this case the content doesn't existe but the connector
			 * has to be called
			 */
			if (response->content != NULL && response->content->length > 0)
			{
				if (!_httpmessage_contentempty(response, 1))
				{

					/**
					 * if for any raison the content_length is not the real
					 * size of the content the following condition must stop
					 * the request
					 */
					response->content_length -=
						(response->content_length < response->content->length)?
						response->content_length : response->content->length;
				}
				sent = _httpclient_sendpart(client, response->content);
				ret = ECONTINUE;
				if (_httpmessage_state(response, PARSE_END))
					_httpmessage_changestate(response, GENERATE_END);
				if (sent == EREJECT)
					ret = EREJECT;
#ifdef DEBUG
				{
					static long long sent = 0;
					sent += response->content->length;
					if (!_httpmessage_contentempty(response, 1) &&
						_httpmessage_state(response, PARSE_END))
						message_dbg("response send content %d %lld %lld", ret, response->content_length, sent);
				}
#endif
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
		}
		break;
		case GENERATE_END:
		{
			if (response->content != NULL && response->content->length > 0)
			{
				_buffer_shrink(response->content, 1);
			}
			http_connector_list_t *callback = request->connector;
			const char *name = "server";
			if (callback)
				name = callback->name;
			warn("response to %p from connector \"%s\" result %d", client, name, request->response->result);
			ret = ESUCCESS;
		}
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
static int _httpclient_run(http_client_t *client)
{
#ifdef DEBUG
	struct timespec spec;
	clock_gettime(CLOCK_MONOTONIC, &spec);
	//dbg("\tclient %p state %X at %d:%d", client, client->state, spec.tv_sec, spec.tv_nsec);
#endif
	int recv_ret = ECONTINUE;
	int send_ret = ECONTINUE;
	int wait_option = 0;

	switch (client->state & CLIENT_MACHINEMASK)
	{
		case CLIENT_NEW:
			wait_option = WAIT_ACCEPT;
		case CLIENT_WAITING:
		{
			recv_ret = _httpclient_wait(client, wait_option);
		}
		break;
		case CLIENT_READING:
		{
			send_ret = ESUCCESS;
			if (_buffer_empty(client->sockdata))
				recv_ret = client->ops->status(client->opsctx);
		}
		break;
		case CLIENT_SENDING:
		{
			send_ret = _httpclient_wait(client, WAIT_SEND);
			if (_buffer_empty(client->sockdata))
				recv_ret = client->ops->status(client->opsctx);
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
			http_client_modctx_t *modctx = client->modctx;
			while (modctx)
			{
				http_client_modctx_t *next = modctx->next;
				dbg("free module instance %s", modctx->name);
				if (modctx->freectx)
				{
					/**
					 * The module may be used by the locked client.
					 * Example: it's forbidden to free TLS while the
					 * client is running
					 * But after is impossible to free the module.
					 */
					modctx->freectx(modctx->ctx);
				}
				free(modctx);
				modctx = next;
			}
			if (!(client->state & CLIENT_LOCKED))
				client->ops->disconnect(client->opsctx);

			client->state |= CLIENT_STOPPED;
			return ESUCCESS;
		}
		break;
		default:
		break;
	}

	if ((recv_ret == ESUCCESS) && !(client->state & CLIENT_LOCKED))
	{
		int size;

		/**
		 * here, it is the call to the recvreq callback from the
		 * server configuration.
		 * see http_server_config_t and httpserver_create
		 */
		_buffer_reset(client->sockdata);
		size = client->client_recv(client->recv_arg, client->sockdata->offset, client->sockdata->size - client->sockdata->length - 1);
		if (size == 0 || size == EINCOMPLETE)
		{
			/**
			 * error on the connection
			 */
			client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
			client->state |= CLIENT_ERROR;
			return ECONTINUE;
		}
		recv_ret = ESUCCESS;
		client->sockdata->length += size;
		client->sockdata->offset[size] = 0;
		/**
		 * the buffer must always be read from the beginning
		 */
		client->sockdata->offset = client->sockdata->data;

		client->state = CLIENT_READING | (client->state & ~CLIENT_MACHINEMASK);
	}
	else if (recv_ret == EREJECT)
	{
		client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
		client->state |= CLIENT_ERROR;
		return ECONTINUE;
	}
	else if (recv_ret == EINCOMPLETE)
	{
		client->state = CLIENT_WAITING | (client->state & ~CLIENT_MACHINEMASK);
	}

	if (!_buffer_empty(client->sockdata))
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
		recv_ret = _httpclient_message(client, client->request);
		switch (recv_ret)
		{
		case ECONTINUE:
		{
			/**
			 * The request is ready to be manipulate by the connectors
			 */
			client->state = CLIENT_WAITING | (client->state & ~CLIENT_MACHINEMASK);
		}
		break;
		case EINCOMPLETE:
		{
			if (_buffer_empty(client->sockdata))
			{
				/**
				 * The request is not ready and need more data
				 */
				client->state = CLIENT_WAITING | (client->state & ~CLIENT_MACHINEMASK);
			}
			else
			{
				client->state = CLIENT_READING | (client->state & ~CLIENT_MACHINEMASK);
			}
		}
		break;
		case EREJECT:
		{
			if (client->request->response == NULL)
				client->request->response = _httpmessage_create(client, client->request);

			client->request->connector = &error_connector;
			_httpmessage_changestate(client->request->response, PARSE_END);
			_httpmessage_changestate(client->request->response, GENERATE_ERROR);
			/**
			 * The format of the request is bad. It may be an attack.
			 */
			warn("bad request");
			client->request->state = PARSE_END;
			/**
			 * The request contains an syntax error and must be rejected
			 */
			client->state = CLIENT_EXIT | CLIENT_LOCKED | (client->state & ~CLIENT_MACHINEMASK);
			_buffer_reset(client->sockdata);
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
				_buffer_shrink(client->sockdata, 1);
			client->request = NULL;
			client->state = CLIENT_SENDING | (client->state & ~CLIENT_MACHINEMASK);
		}
		}
	}

	http_message_t *request = client->request_queue;
	if (request != NULL &&
		((request->state & PARSE_MASK) > PARSE_PRECONTENT))
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
			client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
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
		http_message_t *response = request->response;

		if ((response->state & GENERATE_MASK) > 0)
		{
			int ret = EINCOMPLETE;
			do
			{
				ret = _httpclient_response(client, request);
			} while (ret == EINCOMPLETE);

			if (ret == ESUCCESS)
			{
				ret = ECONTINUE;
				if (_httpmessage_contentempty(response, 1))
				{
					warn("client: disable keep alive (Content-Length is not set)");
					client->state &= ~CLIENT_KEEPALIVE;
				}

				if ((request->state & PARSE_MASK) < PARSE_END)
				{
					client_dbg("client: uncomplete");
					client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
					ret = EINCOMPLETE;
				}
				else if (client->state & CLIENT_LOCKED)
				{
					client_dbg("client: locked");
					client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
				}
				else if (httpmessage_result(request->response, -1) > 399)
				{
					client_dbg("client: exit on result");
					client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
					ret = EINCOMPLETE;
				}
				else if (client->state & CLIENT_KEEPALIVE)
				{
					client_dbg("client: keep alive");
					client->state = CLIENT_READING | (client->state & ~CLIENT_MACHINEMASK);
				}
				else
				{
					client_dbg("client: exit");
					client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
					ret = EINCOMPLETE;
				}
				/**
				 * client->request is not null if the reception is not complete.
				 * In this case the client keeps the request until the connection
				 * is closed
				 */
				warn("client: response complete");
				client->request_queue = request->next;
				_httpmessage_destroy(request);
				return ret;
			}
			else if (ret == EREJECT)
			{
				err("client should exit");
				client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
			}
			else
				client->state = CLIENT_SENDING | (client->state & ~CLIENT_MACHINEMASK);
		}
	}
	return ECONTINUE;
}

void httpclient_shutdown(http_client_t *client)
{
	client->ops->disconnect(client->opsctx);
	client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
}

/***********************************************************************
 * http_server
 */
static int _httpserver_setmod(http_server_t *server, http_client_t *client)
{
	int ret = ESUCCESS;
	http_server_mod_t *mod = server->mod;
	http_client_modctx_t *currentctx = client->modctx;
	while (mod)
	{
		http_client_modctx_t *modctx = vcalloc(1, sizeof(*modctx));
		if (modctx == NULL)
		{
			ret = EREJECT;
			break;
		}
		dbg("new module instance %s", mod->name);
		if (mod->func)
		{
			modctx->ctx = mod->func(mod->arg, client, (struct sockaddr *)&client->addr, client->addr_size);
			if (modctx->ctx == NULL)
				ret = EREJECT;
		}
		modctx->freectx = mod->freectx;
		modctx->name = mod->name;
		mod = mod->next;
		if (client->modctx == NULL)
			client->modctx = modctx;
		else
		{
			currentctx->next = modctx;
		}
		currentctx = modctx;
	}
	return ret;
}

static int _httpserver_prepare(http_server_t *server)
{
	int count = 0;
	int maxfd = 0;

	int checksockets = 1;
	maxfd = server->sock;

	http_client_t *client = server->clients;
#ifndef VTHREAD
	client = server->clients;
	while (client != NULL)
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
		client = client->next;
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

# include <sys/ioctl.h>

static int _httpserver_checkclients(http_server_t *server, fd_set *prfds, fd_set *pwfds, fd_set *pefds)
{
	int ret = 0;
	int run = 0;
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
			client->state |= CLIENT_RUNNING;
			int run_ret;
			do
			{
				int ret;
				run_ret = _httpclient_run(client);
				if (run_ret == ESUCCESS)
					client->state = CLIENT_DEAD | (client->state & ~CLIENT_MACHINEMASK);
			}
			while (run_ret == EINCOMPLETE && client->request_queue == NULL);
			run++;
		}

		if ((client->state & CLIENT_MACHINEMASK) == CLIENT_DEAD)
#else

		if ((!vthread_exist(client->thread)) ||
			((client->state & CLIENT_MACHINEMASK) == CLIENT_DEAD))
#endif
		{
			warn("client %p died", client);
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
			_httpclient_destroy(client);
			client = client2;
		}
		else
		{
			ret++;
			client = client->next;
		}
	}

	return ret;
}

#ifdef DEBUG
static int _debug_nbclients = 0;
static int _debug_maxclients = 0;
#endif
static int _httpserver_checkserver(http_server_t *server, fd_set *prfds, fd_set *pwfds, fd_set *pefds)
{
	int ret = ESUCCESS;
	int count = 0;

	if (server->sock == -1)
		return EREJECT;

	count = _httpserver_checkclients(server, prfds, pwfds, pefds);
#ifdef DEBUG
	_debug_maxclients = (_debug_maxclients > count)? _debug_maxclients: count;
	server_dbg("nb clients %d / %d / %d", count, _debug_maxclients, _debug_nbclients);
#endif

	if (FD_ISSET(server->sock, pefds))
	{
		err("server %p exception", server);
		FD_CLR(server->sock, prfds);
	}

	if ((count + 1) > server->config->maxclients)
	{
		ret = EINCOMPLETE;
		//err("maxclients");
#ifdef VTHREAD
		vthread_yield(server->thread);
#else
		/**
		 * It may be possible to call _httpserver_checkserver
		 * and create a recursion of the function while at least
		 * one client doesn't die. But if the clients never die,
		 * it becomes an infinite loop.
		 */
		ret = _httpserver_checkclients(server, prfds, pwfds, pefds);
#endif
	}
	else if (FD_ISSET(server->sock, prfds))
	{
		http_client_t *client = NULL;
		do
		{
			client = server->ops->createclient(server);

			if (client != NULL)
			{
				ret = _httpserver_setmod(server, client);
#ifdef VTHREAD
				if (ret == ESUCCESS)
				{
					vthread_attr_t attr;
					client->state &= ~CLIENT_STOPPED;
					client->state |= CLIENT_STARTED;
					ret = vthread_create(&client->thread, &attr, (vthread_routine)_httpclient_connect, (void *)client, sizeof(*client));
#ifndef SHARED_SOCKET
					/**
					 * To disallow the reception of SIGPIPE during the
					 * "send" call, the socket into the parent process
					 * must be closed.
					 * Or the tcpserver must disable SIGPIPE
					 * during the sending, but in this case
					 * it is impossible to recceive real SIGPIPE.
					 */
					close(client->sock);
#endif
				}
#endif
				if (ret == ESUCCESS)
				{
					client->next = server->clients;
					server->clients = client;

#ifdef DEBUG
					_debug_nbclients++;
#endif
					count++;
				}
				else
				{
					/**
					 * One module rejected the new client socket.
					 * It may be a bug or a module checking the client
					 * like "clientfilter"
					 */
					httpclient_shutdown(client);
					_httpclient_destroy(client);
				}
			}
		}
		while (client != NULL && count < server->config->maxclients);
		/**
		 * this loop generates more exception on the server socket.
		 * The exception is handled and should not generate trouble.
		 *
		 * this loop cheks there aren't more than one connection in
		 * the same time.
		 * The second "createclient" call generates the message:
		 * "tcpserver accept error Resource temporarily unavailable"
		 */

		if ((count + 1) > server->config->maxclients)
			ret = EINCOMPLETE;
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

static int _httpserver_run(http_server_t *server)
{
	int ret = ESUCCESS;
	int run = 0;

	server->run = 1;
	run = 1;

	warn("server %s %d running", server->config->hostname, server->config->port);
	while(run > 0)
	{
		struct timespec *ptimeout = NULL;
		int maxfd = 0;
		fd_set *prfds, *pwfds, *pefds;
#ifdef USE_POLL
		fd_set rfds, wfds, efds;

		prfds = &rfds;
		pwfds = &wfds;
		pefds = &efds;
#else
		prfds = &server->fds[0];
		pwfds = &server->fds[1];
		pefds = &server->fds[2];
#endif
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

		int nbselect = server->numfds;
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
					FD_SET(server->poll_set[j].fd, &rfds);
					server->poll_set[j].revents &= ~POLLIN;
				}
				if (server->poll_set[j].revents & POLLOUT)
				{
					FD_SET(server->poll_set[j].fd, &wfds);
					server->poll_set[j].revents &= ~POLLOUT;
				}
				if (server->poll_set[j].revents & POLLERR)
				{
					FD_SET(server->poll_set[j].fd, &rfds);
					FD_SET(server->poll_set[j].fd, &efds);
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
						FD_SET(server->poll_set[j].fd, &rfds);
						FD_SET(server->poll_set[j].fd, &efds);
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
			http_client_t *client = server->clients;
			while (client != NULL)
			{
				client->timeout -= WAIT_TIMER * 100;
				if (client->timeout < 0)
					checkclients = 1;
				client = client->next;
			}
			if (checkclients)
				_httpserver_checkclients(server, prfds, pwfds, pefds);
#endif
		}
		else if (nbselect < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
			{
				warn("server %p select error (%d, %s)", server, errno, strerror(errno));
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
					http_client_t *client = server->clients;
					while (client != NULL)
					{
						warn("EBADF %p (%d)", client, client->sock);
						int ret = write(client->sock, NULL, 0);
						warn("EBADF %p (%d)", client, ret);
						client = client->next;
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
			ret = _httpserver_checkserver(server, prfds, pwfds, pefds);
			if (ret == EREJECT)
			{
				server->run = 0;
			}
#ifdef VTHREAD
			vthread_yield(server->thread);
#endif
		}
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
	server->ops = httpserver_ops;
	server->methods = (http_message_method_t *)default_methods;

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

http_server_t *httpserver_dup(http_server_t *server)
{
	http_server_t *vserver;

	vserver = vcalloc(1, sizeof(*vserver));
	if (vserver == NULL)
		return NULL;
	vserver->config = server->config;
	vserver->ops = server->ops;
	vserver->methods = (http_message_method_t *)default_methods;

	vserver->protocol_ops = server->protocol_ops;
	vserver->protocol = server->protocol;

	return vserver;
}

void httpserver_addmethod(http_server_t *server, const char *key, short properties)
{
	short id = 0;
	http_message_method_t *method = server->methods;
	while (method != NULL)
	{
		id = method->id;
		if (!strcmp(method->key, key))
		{
			break;
		}
		method = (http_message_method_t *)method->next;
	}
	if (method == NULL)
	{
		method = vcalloc(1, sizeof(*method));
		if (method == NULL)
			return;
		method->key = key;
		method->id = id + 1;
		method->next = (const http_message_method_t *)server->methods;
		server->methods = method;
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

static void _http_addconnector(http_connector_list_t **first,
						http_connector_t func, void *funcarg,
						int priority, const char *name)
{
	http_connector_list_t *callback;

	callback = vcalloc(1, sizeof(*callback));
	if (callback == NULL)
		return;
	callback->func = func;
	callback->arg = funcarg;
	callback->name = name;
	callback->priority = priority;
	if (*first == NULL)
	{
		*first = callback;
		dbg("install connector %s", callback->name);
	}
	else
	{
		http_connector_list_t *previous = NULL;
		http_connector_list_t *it = *first;
		while (it != NULL && it->priority < callback->priority)
		{
			previous = it;
			it = it->next;
		}
		callback->next = it;
		if (previous == NULL)
		{
#ifdef DEBUG
			if (it != NULL)
				dbg("install connector first =  %s < %s", callback->name, it->name);
			else
				dbg("install connector first =  %s", callback->name);
#endif
			*first = callback;
		}
		else
		{
#ifdef DEBUG
			if (it != NULL)
				dbg("install connector %s < %s < %s", previous->name, callback->name, it->name);
			else
				dbg("install connector %s < %s = end ", previous->name, callback->name);
#endif
			previous->next = callback;
		}
	}
}

void httpserver_addconnector(http_server_t *server,
						http_connector_t func, void *funcarg,
						int priority, const char *name)
{
	_http_addconnector(&server->callbacks, func, funcarg, priority, name);
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

void httpserver_disconnect(http_server_t *server)
{
	server->run = 0;
	server->ops->close(server);
}

void httpserver_destroy(http_server_t *server)
{
#ifdef VTHREAD
	if (server->thread)
	{
		vthread_join(server->thread, NULL);
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
		/**
		 * default_method must not be freed
		 * prefere to have memory leaks
		 */
		/*vfree(method);*/
		method = next;
	}
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
static char service[NI_MAXSERV];
const char *httpserver_INFO(http_server_t *server, const char *key)
{
	const char *value = default_value;

	if (!strcasecmp(key, "name") || !strcasecmp(key, "host") || !strcasecmp(key, "hostname"))
	{
		value = server->config->hostname;
	}
	else if (!strcasecmp(key, "domain"))
	{
		value = strchr(server->config->hostname, '.');
		if (value)
			value ++;
		else
			value = default_value;
	}
	else if (!strcasecmp(key, "software"))
	{
		value = httpserver_software;
	}
	else if (!strcasecmp(key, "scheme"))
	{
		value = server->protocol_ops->scheme;
	}
	else if (!strcasecmp(key, "protocol"))
	{
		value = httpversion[(server->config->version & HTTPVERSION_MASK)];
	}
	else if (!strcasecmp(key, "service"))
	{
		snprintf(service, NI_MAXSERV, "%d", server->protocol_ops->default_port);
		value = service;
	}
	else if (!strcasecmp(key, "secure"))
	{
		if (server->protocol_ops->type & HTTPCLIENT_TYPE_SECURE)
			value = str_true;
		else
			value = str_false;
	}
	else if (!strcasecmp(key, "port"))
	{
#if 1
		if (server->protocol_ops->default_port != server->config->port)
		{
			snprintf(service, NI_MAXSERV, "%d", server->config->port);
			value = service;
		}
#else
		struct sockaddr_in sin;
		socklen_t len = sizeof(sin);
		if (getsockname(server->sock, (struct sockaddr *)&sin, &len) == 0)
		{
			getnameinfo((struct sockaddr *) &sin, len,
				0, 0,
				service, NI_MAXSERV, NI_NUMERICSERV);
			value = service;
		}
#endif
	}
	return value;
}
