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
#include "_httpclient.h"
#include "_httpmessage.h"
#include "_buffer.h"
#include "dbentry.h"

#define client_dbg(...)

static int _httpclient_run(http_client_t *client);
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
	_httpconnector_add(&client->callbacks, func, funcarg, priority, name);
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
		if (size == EREJECT || _httpmessage_state(response, PARSE_END))
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
int _httpclient_thread(http_client_t *client)
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

static int _httpclient_checkconnector(http_client_t *client, http_message_t *request, http_message_t *response, int priority)
{
	int ret = ESUCCESS;
	http_connector_list_t *iterator;
	iterator = client->callbacks;
	while (iterator != NULL)
	{
		if (iterator->func)
		{
			if (priority > 0 && iterator->priority != priority)
			{
				iterator = iterator->next;
				continue;
			}
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
			error_connector.arg = client;
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
				static long long sent = 0;
				sent += response->content->length;
				if (!_httpmessage_contentempty(response, 1) &&
					_httpmessage_state(response, PARSE_END))
				client_dbg("response send content %d %lld %lld", ret, response->content_length, sent);
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
			if (!_buffer_full(client->sockdata))
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
		}
		return ESUCCESS;
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
		_buffer_shrink(client->sockdata, 0);
		size = client->client_recv(client->recv_arg, client->sockdata->offset, client->sockdata->size - client->sockdata->length - 1);
		if (size == 0 || size == EREJECT)
		{
			/**
			 * error on the connection
			 */
			client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
			client->state |= CLIENT_ERROR;
			return ECONTINUE;
		}
		else if (size == EINCOMPLETE)
		{
			client->state = CLIENT_WAITING | (client->state & ~CLIENT_MACHINEMASK);
		}
		else
		{
			recv_ret = ESUCCESS;
			client->sockdata->length += size;
			client->sockdata->offset[size] = 0;
			/**
			 * the buffer must always be read from the beginning
			 */
			client->sockdata->offset = client->sockdata->data;

			client->state = CLIENT_READING | (client->state & ~CLIENT_MACHINEMASK);
		}
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

			error_connector.arg = client;
			client->request->connector = &error_connector;
			_httpmessage_changestate(client->request->response, PARSE_CONTENT);
			client->request->response->state |= PARSE_CONTINUE;
			_httpmessage_changestate(client->request->response, GENERATE_ERROR);
			/**
			 * The format of the request is bad. It may be an attack.
			 */
			warn("bad request");
			_httpmessage_changestate(client->request, PARSE_END);
			/**
			 * The request contains an syntax error and must be rejected
			 */
			client->state = CLIENT_READING | (client->state & ~CLIENT_MACHINEMASK);
			_buffer_reset(client->sockdata);
			client->request = NULL;
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
