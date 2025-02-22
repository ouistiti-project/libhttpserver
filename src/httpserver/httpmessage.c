/*****************************************************************************
 * httpmessage.c: Simple HTTP server
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
#include "ouistiti/log.h"
#include "ouistiti/httpserver.h"
#include "_httpserver.h"
#include "_httpclient.h"
#define _HTTPMESSAGE_
#include "_httpmessage.h"
#include "_buffer.h"
#include "dbentry.h"

#define buffer_dbg(...)
#define message_dbg(...)
#define client_dbg(...)
#define server_dbg(...)

static string_t httpversion[HTTPVERSIONS] =
{
	STRING_DCL("HTTP/0.9"),
	STRING_DCL("HTTP/1.0"),
	STRING_DCL("HTTP/1.1"),
	STRING_DCL("HTTP/2"),
};

const char str_get[] = "GET";
const char str_post[] = "POST";
const char str_head[] = "HEAD";
const char str_form_urlencoded[] = "application/x-www-form-urlencoded";
const char str_cookie[] = "Cookie";
const char str_connection[] = "Connection";
const char str_contenttype[] = "Content-Type";
const char str_contentlength[] = "Content-Length";
const char str_keepalive[] = "Keep-Alive";
const char str_setcookie[] = "Set-Cookie";

const char str_uri[] = "uri";
const char str_query[] = "query";
const char str_content[] = "content";
static const char str_headerstorage[] = "headerstorage";

const http_message_method_t default_methods[] = {
	{ .key = STRING_DCL(str_get), .id = MESSAGE_TYPE_GET, .next = (http_message_method_t *)&default_methods[1]},
	{ .key = STRING_DCL(str_post), .id = MESSAGE_TYPE_POST, .properties = MESSAGE_ALLOW_CONTENT, .next =(http_message_method_t *) &default_methods[2]},
	{ .key = STRING_DCL(str_head), .id = MESSAGE_TYPE_HEAD, .next = NULL},
#ifdef HTTPCLIENT_FEATURES
	{ .key = {NULL, 0, 0}, .id = -1, .next = NULL},
#endif
};
extern ssize_t tcpserver_getname(struct sockaddr_storage *addr, socklen_t addrlen, char *buffer, size_t length, int flag);

size_t httpserver_version(http_message_version_e versionid, const char **version)
{
	if (version)
		*version = NULL;
	if ((versionid & HTTPVERSION_MASK) < HTTPVERSIONS)
	{
		if (version)
			*version = _string_get(&httpversion[versionid & HTTPVERSION_MASK]);
		return _string_length(&httpversion[versionid & HTTPVERSION_MASK]);
	}
	return 0;
}

int httpmessage_chunksize()
{
	return _buffer_chunksize(-1);
}

#ifdef HTTPCLIENT_FEATURES
http_message_t * httpmessage_create()
{
	http_message_t *client = _httpmessage_create(NULL, NULL);
	return client;
}

void httpmessage_destroy(http_message_t *message)
{
	_httpmessage_destroy(message);
}

http_client_t *httpmessage_request(http_message_t *message, const char *method, const char *url, ...)
{
	http_client_t *client = NULL;
	const http_message_method_t *method_it;
	for (method_it = &default_methods[0]; method_it != NULL; method_it = method_it->next)
	{
		if (!_string_cmp(&method_it->key, method, -1))
			break;
	}
	if (method_it == NULL)
	{
		method_it = &default_methods[3];
		_string_store(&((http_message_method_t *)method_it)->key, method, -1);
	}
	message->method = method_it;
	message->version = HTTP11;
	message->state = GENERATE_INIT;

	char *host = NULL;
	if (url)
		host = strstr(url, "://");
	if (host)
	{
		int schemelength = host - url;
		host += 3;
		char *pathname = strchr(host, '/');
		if (pathname == NULL)
			return NULL;
		*pathname = 0;
		pathname++;

		const httpclient_ops_t *protocol_ops = NULL;
		void *protocol_ctx = NULL;
		for (const httpclient_ops_t *it_ops = httpclient_ops();it_ops != NULL; it_ops = it_ops->next)
		{
			if (!strncmp(url, it_ops->scheme, schemelength))
			{
				protocol_ops = it_ops;
				protocol_ctx = client;
				break;
			}
		}
		if (protocol_ops == NULL)
			return NULL;
		int iport = protocol_ops->default_port;

		char *port = strchr(host, ':');
		if (port != NULL && port < pathname)
		{
			*port = 0;
			port++;
			iport = atoi(port);
		}

		httpmessage_addheader(message, "Host", host, strlen(host));
		client = httpclient_create(NULL, protocol_ops, protocol_ctx);
		int ret = _httpclient_connect(client, host, iport);
		if (ret == EREJECT)
		{
			err("client connection error");
			httpclient_destroy(client);
			client = NULL;
		}
		else
			url = pathname;
	}
	if (url)
	{
		va_list argv;
		int length = strlen(url);
		const char *s = NULL;

		va_start(argv, url);
		s = va_arg(argv, const char *);
		while (s != NULL)
		{
			length += strlen(s);
			s = va_arg(argv, const char *);
		}
		va_end(argv);

		int nbchunks = (length / _buffer_chunksize(-1)) + 1;
		message->uri = _buffer_create(str_uri, nbchunks);

		_buffer_append(message->uri, url, -1);
		va_start(argv, url);
		s = va_arg(argv, const char *);
		while (s != NULL)
		{
			_buffer_append(message->uri, s, -1);
			s = va_arg(argv, const char *);
		}
		va_end(argv);
	}
	return client;
}
#endif

http_message_t * _httpmessage_create(http_client_t *client, http_message_t *parent)
{
	http_message_t *message;

	message = vcalloc(1, sizeof(*message));
	if (message)
	{
		message->result = RESULT_200;
		message->client = client;
		message->content_length = (unsigned long long)-1;
		message->version = -1;
		if (parent)
		{
			parent->response = message;

			message->method = parent->method;
			message->client = parent->client;
			message->version = parent->version;
			message->result = parent->result;
			message->mode = parent->mode;
		}
	}
	return message;
}

void _httpmessage_destroy(http_message_t *message)
{
	if (message->response)
	{
		_httpmessage_destroy(message->response);
	}
	if (message->uri)
		_buffer_destroy(message->uri);
	if (message->content_storage)
		_buffer_destroy(message->content_storage);
	if (message->header)
		_buffer_destroy(message->header);
	if (message->headers)
		dbentry_destroy(message->headers);
	if (message->headers_storage)
		_buffer_destroy(message->headers_storage);
	if (message->query_storage)
		_buffer_destroy(message->query_storage);
	if (message->queries)
		dbentry_destroy(message->queries);
	if (message->cookie_storage)
		_buffer_destroy(message->cookie_storage);
	dbentry_destroy(message->cookies);
	vfree(message);
}

int _httpmessage_changestate(http_message_t *message, int new)
{
	int mask = PARSE_MASK;
	if (new & GENERATE_MASK)
		mask = GENERATE_MASK;
	message->state = new | (message->state & ~mask);
	return message->state;
}

int _httpmessage_state(http_message_t *message, int check)
{
	int mask = PARSE_MASK;
	if (check & GENERATE_MASK)
		mask = GENERATE_MASK;
	return ((message->state & mask) == check);
}

int _httpmessage_contentempty(http_message_t *message, int unset)
{
	if (unset)
		return (message->content_length == ((unsigned long long) -1));
	return (message->content_length == 0);
}

static int _httpmessage_decodeuri(const char *data, char *decodeval)
{
	const char *encoded = data;
	if (*encoded == '%')
		encoded ++;
	*decodeval = 0;
	for (int i = 0; i < 2; i++)
	{
		*decodeval = *decodeval << 4;
		if (*encoded > 0x29 && *encoded < 0x40)
			*decodeval += (*encoded - 0x30);
		else if (*encoded > 0x40 && *encoded < 0x47)
			*decodeval += (*encoded - 0x41 + 10);
		else if (*encoded > 0x60 && *encoded < 0x67)
			*decodeval += (*encoded - 0x61 + 10);
		else
		{
			*decodeval = -1; ///error
			break;
		}
		encoded ++;
	}
	return encoded - data;
}

static int _httpmesssage_parsefailed(http_message_t *message)
{
	message->version = httpclient_server(message->client)->config->version;
	switch (message->state  & PARSE_MASK)
	{
#ifdef RESULT_405
	case PARSE_INIT:
		message->result = RESULT_405;
	break;
#endif
#ifdef RESULT_414
	case PARSE_URI:
		message->result = RESULT_414;
	break;
#endif
	default:
		message->result = RESULT_400;
	break;
	}

	return PARSE_END;
}

static int _httpmessage_parseinit(http_message_t *message, buffer_t *data)
{
	int next = PARSE_INIT;

	for (const http_message_method_t *method = httpclient_server(message->client)->methods; method != NULL; method = method->next)
	{
		size_t length = _string_length(&method->key);
		if (!_string_cmp(&method->key, data->offset, -1) &&
			data->offset[length] == ' ')
		{
			message->method = method;
			data->offset += length + 1;
			next = PARSE_URI;
			/**
			 * to parse a request the default value of content_length MUST be 0
			 * otherwise the parser continue to wait content.
			 * for GET method, there isn't any content and content_length is not set
			 */
			message->content_length = 0;
			break;
		}
	}

	if (message->method == NULL)
	{
		err("message: reject method %s", data->offset);
		data->offset++;
		next = _httpmesssage_parsefailed(message);
	}
	return next;
}

static int _httpmessage_pushuri(http_message_t *message, int next, const char *uri, size_t length)
{
	if (length > 0)
	{
		int offset = _buffer_append(message->uri, uri, length);
		if (offset < 0)
		{
			next = _httpmesssage_parsefailed(message);
			err("message: reject uri too long : %s", _buffer_get(message->uri, 0));
		}
	}
	return next;
}

static int _httpmessage_parseuri(http_message_t *message, buffer_t *data)
{
	int next = PARSE_URI;
	const char *uri = data->offset;
	size_t length = 0;

	if (message->uri == NULL)
	{
		if (uri[0] == '/')
			message->uri = _buffer_create(str_uri, MAXCHUNKS_URI);
		/**
		 * empty URI is accepted
		 */
		else if (uri[0] == ' ')
			message->uri = _buffer_create(str_uri, MAXCHUNKS_URI);
		else if (uri[0] == '%')
			message->uri = _buffer_create(str_uri, MAXCHUNKS_URI);
		else if (uri[0] == '\r')
			message->uri = _buffer_create(str_uri, MAXCHUNKS_URI);
		else if (uri[0] == '\n')
			message->uri = _buffer_create(str_uri, MAXCHUNKS_URI);
		else
			next = _httpmesssage_parsefailed(message);
	}

	while (data->offset < (_buffer_get(data, 0) + _buffer_length(data)) && next == PARSE_URI)
	{
		if (*(data->offset + 1) == '\0')
		{
			next = PARSE_URI | PARSE_CONTINUE;
			break;
		}
		switch (*data->offset)
		{
#ifndef HTTPMESSAGE_NODOUBLEDOT
			case '.':
			{
				if (*(data->offset + 1) == '.')
				{
					next = _httpmessage_pushuri(message, next, uri, length);
					if (_buffer_rewindto(message->uri, '/') != ESUCCESS)
					{
						next = _httpmesssage_parsefailed(message);
						err("message: reject dangerous uri : %s", _buffer_get(data, 0));
						break;
					}

					if (_buffer_rewindto(message->uri, '/') != ESUCCESS)
					{
						next = _httpmesssage_parsefailed(message);
						err("message: reject dangerous uri : %s", _buffer_get(data, 0));
						break;
					}
					data->offset += 2 - 1;
					length = 0;
					uri = data->offset + 1;
				}
				else
					length++;
			}
			break;
#endif
			case '%':
			{
				next = _httpmessage_pushuri(message, next, uri, length);
				char code = 0;
				int ret = _httpmessage_decodeuri(data->offset, &code);
				if (code == -1)
				{
					next = _httpmesssage_parsefailed(message);
					err("message: reject uri mal formated : %s %s",
							uri,
							_buffer_get(data, 0));
				}
				else
					next = _httpmessage_pushuri(message, next, &code, 1);
				if (ret > 0)
					data->offset += ret - 1;
				length = 0;
				uri = data->offset + 1;
			}
			break;
			case '/':
				/**
				 * Remove all double / inside the URI.
				 * This may allow unsecure path with double .
				 * But leave double // for the query part
				 */
				length++;
				if (*(data->offset + 1) == '/')
				{
					next = _httpmessage_pushuri(message, next, uri, length);
				}
				while (*(data->offset + 1) == '/')
				{
					data->offset++;
					uri = data->offset + 1;
					length = 0;
				}
			break;
			case '?':
				next = PARSE_QUERY;
			break;
#ifdef MESSAGE_URIFRAGID
			case '#':
				next = PARSE_URIFRAGID;
			break;
#endif
			case ' ':
				next = PARSE_VERSION;
			break;
			case '*':
				length++;
			break;
			case '\r':
			case '\n':
			{
				/// version is not present but it must be parse to be "HTTP/0.9"
				next = PARSE_END;
				if (*(data->offset + 1) == '\n')
				{
					data->offset++;
				}
				message->version = 0;
			}
			break;
			default:
			{
				if ((*data->offset) < 0x19)
				{
					next = _httpmesssage_parsefailed(message);
					err("message: reject bad chararcter into uri : %s", _buffer_get(data, 0));
					break;
				}
				else
					length++;
			}
		}
		data->offset++;
	}
	next = _httpmessage_pushuri(message, next, uri, length);
	return next;
}

#ifdef MESSAGE_URIFRAGID
static int _httpmessage_parseurifragid(http_message_t *message, buffer_t *data)
{
	int next = PARSE_URIFRAGID;
	int length = 0;
	const char *uri = data->offset;
	while (data->offset < (_buffer_get(data, 0) + _buffer_length(data)) && next == PARSE_URIFRAGID)
	{
		switch (*data->offset)
		{
			case '?':
				next = PARSE_QUERY;
			break;
			case ' ':
				next = PARSE_VERSION;
			break;
			case '\r':
			case '\n':
			{
				next = PARSE_PREHEADER;
				if (*(data->offset + 1) == '\n')
					data->offset++;
			}
		}
		if (next != (PARSE_URIFRAGID | PARSE_CONTINUE))
			data->offset++;
	}

	return next;
}
#endif

static int _httpmessage_parsequery(http_message_t *message, buffer_t *data)
{
	int next = PARSE_QUERY;
	int length = 0;
	const char *query = data->offset;

	if (message->query_storage == NULL)
	{
		message->query_storage = _buffer_create(str_query, MAXCHUNKS_URI);
	}

	while (data->offset < (data->data + data->length) && next == PARSE_QUERY)
	{
		switch (*data->offset)
		{
			case ' ':
			{
				next = PARSE_VERSION;
			}
			break;
			case '\r':
			case '\n':
			{
				next = PARSE_PREHEADER;
				if (*(data->offset + 1) == '\n')
					data->offset++;
			}
			break;
			default:
			{
				length++;
			}
		}
		data->offset++;
	}

	if (length > 0 && (message->query_storage != NULL))
	{
		int offset = _buffer_append(message->query_storage, query, length);
		next &= ~PARSE_CONTINUE;
		if (offset < 0)
		{
			next = _httpmesssage_parsefailed(message);
			err("message: reject query too long : %s %s",
					_buffer_get(message->query_storage, 0),
					_buffer_get(data, 0));
		}
	}
	return next;
}

static int _httpmessage_parsestatus(http_message_t *message, buffer_t *data)
{
	int next = PARSE_STATUS;
	int version = -1;
	for (int i = 0; i < sizeof(httpversion)/sizeof(string_t); i++)
	{
		if (!_string_cmp(&httpversion[i], data->offset, -1))
		{
			version = i;
			message->version = i;
			data->offset += _string_length(&httpversion[i]);
			data->offset++;
			break;
		}
	}
	if (version != -1)
	{
		/** pass the next space character */
		message->result = strtol(data->offset, &data->offset, 10);
		char status[4] = {0};
		int len = snprintf(status, 4, "%.3d", message->result);
		httpmessage_addheader(message, "Status", status, len);
	}
	else /// the error is normal for CGI
		err("message: protocol version not supported %s", data->offset);
	next = PARSE_HEADER;
	return next;
}

static int _httpmessage_parseversion(http_message_t *message, buffer_t *data)
{
	int next = PARSE_VERSION;

	/**
	 * There is not enougth data to parse the version.
	 * Move the rest to the beginning and request
	 * more data.
	 **/
	if (data->offset + 10 > _buffer_get(data, 0) + _buffer_length(data))
	{
		return next;
	}
	char *version = data->offset;
	for (int i = 0; i < sizeof(httpversion)/sizeof(string_t); i++)
	{
		if (!_string_cmp(&httpversion[i], version, -1))
		{
			data->offset += _string_length(&httpversion[i]);
			if (*data->offset == '\r')
				data->offset++;
			if (*data->offset == '\n')
			{
				data->offset++;
				next = PARSE_PREHEADER;
			}
			else
			{
				next = PARSE_END;
				message->result = RESULT_400;
				err("bad request %s", data->data);
			}
			message->version = i;
			break;
		}
	}
	if (message->version == -1)
	{
		next = _httpmesssage_parsefailed(message);
		err("message: bad protocol version %s", version);
	}
	return next;
}

static int _httpmessage_parsepreheader(http_message_t *message, buffer_t *data)
{
	int next = PARSE_HEADER;
	/**
	 * keep the query to the end of the URI first.
	 * When the URI is completed, remove the '?'
	 * and update the query pointer
	 **/
	if (message->uri != NULL && message->uri->length > 0)
	{
		/**
		 * query must be set at the end of the uri loading
		 * uri buffer may be change during an extension
		 */
		const char *service = httpserver_INFO(httpclient_server(message->client), "service");
		warn("new request %.*s %.*s from \"%s\" service",
				(int)_string_length(&message->method->key), _string_get(&message->method->key),
				(int)_buffer_length(message->uri), _buffer_get(message->uri, 0), service);
	}
	else if (message->uri == NULL)
	{
		next = _httpmesssage_parsefailed(message);
		err("message: reject URI bad formatting: %s", data->data);
	}
	return next;
}

static int _httpmessage_parseheader(http_message_t *message, buffer_t *data)
{
	int next = PARSE_HEADER;
	char *header = data->offset;
	int length = 0;

	if (message->headers_storage == NULL)
	{
		message->headers_storage = _buffer_create(str_headerstorage, MAXCHUNKS_HEADER);
	}

	/* store header line as "<key>:<value>\0" */
	while (data->offset < (_buffer_get(data, 0) + _buffer_length(data)) && next == PARSE_HEADER)
	{
		switch (*data->offset)
		{
		case '\n':
		{
			/**
			 * Empty Header line defines the end of the header and
			 * the beginning fo the content.
			 **/
			if (length == 0 && !(message->state & PARSE_CONTINUE))
			{
				next = PARSE_POSTHEADER;
			}
			else
			{
				header[length] = '\0';
				if (_buffer_append(message->headers_storage, header, length + 1) < 0)
				{
					next = _httpmesssage_parsefailed(message);
					err("message: header too long!!!");
				}
				else
				{
					header = data->offset + 1;
					length = 0;
					message->state &= ~PARSE_CONTINUE;
				}
			}
		}
		break;
		case '\r':
		break;
		default:
			length++;
		}
		data->offset++;
	}
	/* not enougth data to complete the line */
	if (next == PARSE_HEADER && length > 0)
	{
		_buffer_append(message->headers_storage, header, length);
		message->state |= PARSE_CONTINUE;
	}
	return next;
}

static int _httpmessage_parsepostheader(http_message_t *message, buffer_t *data)
{
	int next = PARSE_POSTHEADER;
	/**
	 * If the client send headers with only \n at end of each line
	 * it is impossible to rebuild the header correctly.
	 * This null character allows to add \r\n at the end of the headers.
	 */
	_buffer_append(message->headers_storage, "\0", 1);
	if (_httpmessage_fillheaderdb(message) != ESUCCESS)
	{
		next = _httpmesssage_parsefailed(message);
		err("message: request bad header %s", _buffer_get(message->headers_storage, 0));
	}
	else
	{
#ifdef DEBUG
		for (dbentry_t *entry = message->headers; entry != NULL; entry = entry->next)
		{
			dbg("message: headers %s", entry->storage->data + entry->key.offset);
		}
#endif
		_buffer_shrink(data);
		next = PARSE_PRECONTENT;
		message->state &= ~PARSE_CONTINUE;
	}
	return next;
}

static int _httpmessage_parseprecontent(http_message_t *message, buffer_t *data)
{
	int next = PARSE_PRECONTENT;

	message->content_packet = 0;
	const char *content_type = NULL;
	int length = 0;
	length = dbentry_search(message->headers, str_contenttype, &content_type);
	if (length > 0)
	{
		const char *end = strchr(content_type, ';');
		if (end)
			length = end - content_type;
	}

	if ((message->method->properties & MESSAGE_ALLOW_CONTENT) &&
			content_type != NULL && !strncasecmp(content_type, str_form_urlencoded, length))
	{
		/**
		 * message mix query data inside the URI and Content
		 */
		if (message->query_storage == NULL)
		{
			int nbchunks = MAXCHUNKS_HEADER;
#ifdef HTTPMESSAGE_QUERY_UNLIMITED
			if (!_httpmessage_contentempty(message, 1))
				nbchunks = (message->content_length / _buffer_chunksize(-1) ) + 1;
#endif
			message->query_storage = _buffer_create(str_query, nbchunks);
		}
		else
		{
			_buffer_append(message->query_storage, "&", 1);
		}
		next = PARSE_POSTCONTENT;
		message->state &= ~PARSE_CONTINUE;
	}
	else if (_httpmessage_contentempty(message, 0))
	{
		next = PARSE_END;
		dbg("no content inside request");
	}
	else
	{
		next = PARSE_CONTENT;
		message->state &= ~PARSE_CONTINUE;
	}

	return next;
}

static int _httpmessage_parsecontent(http_message_t *message, buffer_t *data)
{
	int next = PARSE_CONTENT;

	if (_httpmessage_contentempty(message, 0))
	{
		next = PARSE_END;
	}
	else
	{
		/**
		 * The content of the request is the buffer past to the socket.
		 * Then it is always a partial content. And the content has
		 * to be treat part by part.
		 * Is possible to use the buffer with the function
		 *  httpmessage_content
		 */
		size_t length = _buffer_length(data);
		message_dbg("message: content (%lu): %.*s", length, (int)length, data->offset);

		/**
		 * If the Content-Length header is not set,
		 * the parser must continue while the socket is opened
		 */
		if (_httpmessage_contentempty(message, 1))
		{
			length -= (data->offset - _buffer_get(data, 0));
		}
		/**
		 * At the end of the parsing the content_length of request
		 * is zero. But it is false, the true value is
		 * Sum(content->length);
		 */
		else if (message->content_length <= length)
		{
			/**
			 * the last chunk of the content may contain data from the next request.
			 * The length of the buffer "content" may be larger than the content, but
			 * httpmessage_content must return the length of the content and not more
			 */
			length = message->content_length;
			next = PARSE_END;
		}
		else
		{
			length -= (data->offset - _buffer_get(data, 0));
		}

		if (message->content == NULL)
		{
			message->content_storage = _buffer_create(str_content, 1);
			message->content = message->content_storage;
		}
		_buffer_reset(message->content, 0);
		if (message->content != data)
			_buffer_append(message->content, data->offset, length);
		message->content_packet = length;
		data->offset += length;
	}
	return next;
}

static int _httpmessage_parsepostcontent(http_message_t *message, buffer_t *data)
{
	int next = PARSE_POSTCONTENT;
	const char *query = data->offset;
	size_t length = _buffer_length(data) - (data->offset - _buffer_get(data, 0));
	int offset = _buffer_append(message->query_storage, query, length);
	if (offset < 0)
	{
		next = _httpmesssage_parsefailed(message);
		err("message: reject query too long : %s %s",
				_buffer_get(message->query_storage, 0),
				_buffer_get(data, 0));
	}
	else if (message->content_length <= length)
	{
		/// The content may be binary data like a file
		/// No parsinng must be done
		data->offset += message->content_length;
		message->content = message->query_storage;
		message->content_packet = _buffer_length(message->query_storage);
		message->content_length = message->content_packet;
		next = PARSE_END;
	}
	else
	{
		data->offset += length;
		message->content_length -= length;
		message->state |= PARSE_CONTINUE;
	}
	return next;
}

/**
 * @brief this function parse several data chunk to extract elements of the request.
 *
 * "parserequest" is able to reconstitue the request, and read
 *  - method (GET, HEAD, DELETE, PUT, OPTIONS...)
 *  - version (HTTP0.9 HTTP1.0 HTTP1.1)
 *  - header elements (ContentType, ContentLength, UserAgent...)
 *  - content
 * The element may be retreive with the httpmessage_REQUEST.
 * The function will returns when the header is completed without treat the rest of the chunck.
 * The next call should contain this rest for the content parsing.
 *
 * @param message the structure to fill.
 * @param data the buffer containing the chunk of the request.
 *
 * @return EINCOMPLETE : the request received is to small and the header is not fully received.
 * ECONTINUE : the header of the request is complete and may be use to begin the treatment.
 * ESUCCESS : the content is fully received and the next chunk is not a part of this request.
 * EREJECT : the chunck contains a syntax error, message->result may be use for more information.
 */
int _httpmessage_parserequest(http_message_t *message, buffer_t *data)
{
	int ret = ECONTINUE;

	do
	{
		ret = ECONTINUE;
		int next = message->state  & PARSE_MASK;

		switch (next)
		{
			case PARSE_INIT:
			{
				next = _httpmessage_parseinit(message, data);
			}
			break;
			case PARSE_URI:
			{
				next = _httpmessage_parseuri(message, data);
			}
			break;
#ifdef MESSAGE_URIFRAGID
			case PARSE_URIFRAGID:
			{
				next = _httpmessage_parseurifragid(message, data);
			}
			break;
#endif
			case PARSE_QUERY:
			{
				next = _httpmessage_parsequery(message, data);
			}
			break;
			case PARSE_VERSION:
			{
				next = _httpmessage_parseversion(message, data);
			}
			break;
			case PARSE_STATUS:
			{
				next = _httpmessage_parsestatus(message, data);
			}
			break;
			case PARSE_PREHEADER:
			{
				next = _httpmessage_parsepreheader(message, data);
			}
			break;
			case PARSE_HEADER:
			{
				next = _httpmessage_parseheader(message, data);
			}
			break;
			case PARSE_POSTHEADER:
			{
				next = _httpmessage_parsepostheader(message, data);
			}
			break;
			case PARSE_PRECONTENT:
			{
				next = _httpmessage_parseprecontent(message, data);
			}
			break;
			case PARSE_CONTENT:
			{
				next = _httpmessage_parsecontent(message, data);
			}
			break;
			case PARSE_POSTCONTENT:
			{
				next = _httpmessage_parsepostcontent(message, data);
			}
			break;
			case PARSE_END:
			{
				message_dbg("parse end with %lu data: %s", _buffer_length(data), data->offset);

				if (message->result == RESULT_200)
					ret = ESUCCESS;
				else
					ret = EREJECT;
			}
			break;
			default:
				err("httpmessage: bad state internal error");
			break;
		}
		if (((next & PARSE_MASK) == (message->state & PARSE_MASK)) && (ret == ECONTINUE))
		{
			if ((next & PARSE_MASK) < PARSE_CONTENT)
				ret = EINCOMPLETE;
			break; // get out of the while (ret == ECONTINUE) loop
		}
		message->state = (message->state & ~PARSE_MASK) | (next & PARSE_MASK);
	} while (ret == ECONTINUE);
	return ret;
}

int _httpmessage_buildresponse(http_message_t *message, int version, buffer_t *header)
{
	http_message_version_e _version = message->version;
	if (message->version > (version & HTTPVERSION_MASK))
		_version = (version & HTTPVERSION_MASK);
	_buffer_append(header, _string_get(&httpversion[_version]), _string_length(&httpversion[_version]));

	char status[_HTTPMESSAGE_RESULT_MAXLEN];
	size_t len = _httpmessage_status(message, status, _HTTPMESSAGE_RESULT_MAXLEN);
	_buffer_append(header, status, len);
	_buffer_append(header, "\r\n", 2);

	if (message->result > 399)
		message->mode &= ~HTTPMESSAGE_KEEPALIVE;
	header->offset = (char *)_buffer_get(header, 0);
	_httpmessage_changestate(message, GENERATE_RESULT);
	return ESUCCESS;
}

buffer_t *_httpmessage_buildheader(http_message_t *message)
{
	int headers_contains_length = 0;
	if (message->headers != NULL)
	{
		headers_contains_length = dbentry_search(message->headers, str_contentlength, NULL);
		_buffer_serializedb(message->headers_storage, message->headers, ':', '\n');
		dbentry_destroy(message->headers);
		message->headers = NULL;
	}
	if (!_httpmessage_contentempty(message, 1) && ! headers_contains_length)
	{
		char content_length[32];
		int length = snprintf(content_length, 31, "%llu",  message->content_length);
		httpmessage_addheader(message, str_contentlength, content_length, length);
	}
	if ((message->mode & HTTPMESSAGE_KEEPALIVE) > 0)
	{
		httpmessage_addheader(message, str_connection, STRING_REF(str_keepalive));
	}
	else
	{
		httpmessage_addheader(message, str_connection, STRING_REF("Close"));
	}
	if (message->complete)
	{
		/**
		 * rebuild temporarily the DB for the connectors
		 */
		_buffer_filldb(message->headers_storage, &message->headers, ':', '\r');
		for (http_connector_list_t *it = message->complete; it != NULL;
			it = it->nextcomplete)
		{
			if (it->func)
			{
				message_dbg("message %p complete connector \"%s\"", message->client, it->name);
				it->func(it->arg, NULL, message);
			}
		}
		_buffer_serializedb(message->headers_storage, message->headers, ':', '\n');
		dbentry_destroy(message->headers);
		message->headers = NULL;
	}
	message->headers_storage->offset = (char *)_buffer_get(message->headers_storage, 0);
	return message->headers_storage;
}

void *httpmessage_private(http_message_t *message, void *data)
{
	if (data != NULL)
	{
		message->private = data;
	}
	return message->private;
}

http_client_t *httpmessage_client(http_message_t *message)
{
	return message->client;
}

int httpmessage_content(http_message_t *message, const char **data, size_t *content_length)
{
	int size = 0;
	int state = message->state & PARSE_MASK;
	if (content_length != NULL)
	{
		if (!_httpmessage_contentempty(message, 1))
		{
			*content_length = message->content_length;
		}
		else
		{
			*content_length = 0;
		}
	}
	if (message->content)
	{
		size = message->content_packet;
		if (data)
		{
			*data = _buffer_get(message->content, 0);
		}
		if (data && !_httpmessage_contentempty(message, 1))
			message->content_length -= message->content_packet;
	}
	if ((message->state & GENERATE_MASK) != 0)
		return size;
	if (state < PARSE_CONTENT)
		return EINCOMPLETE;
	/// the socket is already open but no data are ready
	if (size == 0 && state < PARSE_END)
		return ECONTINUE;
	if (httpclient_state(message->client, -1) & CLIENT_STOPPED)
		return EREJECT;
	return size;
}

/**
 * @brief this function is symetric of "parserequest" to read a response
 */
int httpmessage_parsecgi(http_message_t *message, char *data, int *size)
{
	if (_httpmessage_state(message, PARSE_END))
	{
		return ESUCCESS;
	}
	if (data == NULL)
	{
		message->content = NULL;
		_httpmessage_changestate(message, PARSE_END);
		return EINCOMPLETE;
	}
	static buffer_t tempo;
	tempo.data = data;
	tempo.offset = data;
	tempo.length = *size;
	tempo.size = *size;
	if (_httpmessage_state(message, PARSE_INIT))
		message->state = PARSE_STATUS;
	if (_httpmessage_contentempty(message, 0))
		message->content_length = (unsigned long long)-1;

	int ret;
	do
	{
		ret = _httpmessage_parserequest(message, &tempo);
	} while (_httpmessage_state(message, PARSE_PRECONTENT));
	if ((message->state & PARSE_MASK) > PARSE_POSTHEADER)
	{
		*size = tempo.length;
	}
	else
		*size = 0;
	if (_httpmessage_state(message, PARSE_END))
	{
		if (*size > 0)
		{
			*size = 0;
		}
		ret = ECONTINUE;
	}
	return ret;
}

http_message_result_e httpmessage_result(http_message_t *message, http_message_result_e result)
{
	if (result > 0)
		message->result = result;
	return message->result;
}

size_t _httpmessage_status(const http_message_t *message, char *status, size_t statuslen)
{
	int i = 0;
	while (_http_message_result[i] != NULL)
	{
		if (_http_message_result[i]->result == message->result)
		{
			if (status != NULL)
			{
				statuslen = (_string_length(&_http_message_result[i]->status) > statuslen)? statuslen: _string_length(&_http_message_result[i]->status) + 1;
				memcpy(status, _string_get(&_http_message_result[i]->status), statuslen);
			}
			return _string_length(&_http_message_result[i]->status);
		}
		i++;
	}
	if (status != NULL && statuslen > 4)
	{
		snprintf(status, 5, " %.3d", message->result);
	}
	return 4;
}

int _httpmessage_fillheaderdb(http_message_t *message)
{
	_buffer_filldb(message->headers_storage, &message->headers, ':', '\n');
	const char *value = NULL;
	ssize_t valuelen = 0;
	/// HTTP 0.9 and 1.0 must close connection
	if (message->version == 2)
		valuelen = dbentry_search(message->headers, str_connection, &value);
	if (valuelen > 0)
	{
		for (int i = 0; i < valuelen; i++)
		{
			if (!strncasecmp( value + i, STRING_REF(str_keepalive)))
			{
#ifdef HTTPMESSAGE_KEEPALIVE_ENABLED
				message->mode |= HTTPMESSAGE_KEEPALIVE;
#endif
			}
			if (!strncasecmp( value + i, STRING_REF(str_upgrade)))
			{
				warn("Connection upgrading");
				message->mode |= HTTPMESSAGE_LOCKED;
			}
		}
	}
	valuelen = dbentry_search(message->headers, str_contentlength, &value);
	char *endvalue;
	if (valuelen > 0)
	{
		long intvalue = strtol(value, &endvalue, 10);
		if (endvalue - value == valuelen)
			message->content_length = intvalue;
	}
	valuelen = dbentry_search(message->headers, "Status", &value);
	if (valuelen > 0)
	{
		long intvalue = strtol(value, &endvalue, 10);
		if (endvalue - value == valuelen)
			httpmessage_result(message, intvalue);
	}
	valuelen = dbentry_search(message->headers, str_cookie, &value);
	if ((valuelen > 0) && (message->cookies == NULL))
	{
		int nbchunks = ((valuelen + 1) / _buffer_chunksize(-1)) + 1;
		message->cookie_storage = _buffer_create(str_cookie, nbchunks);
		_buffer_append(message->cookie_storage, value, valuelen);
		_buffer_filldb(message->cookie_storage, &message->cookies, '=', ';');
	}
	return ESUCCESS;
}

int _httpmessage_runconnector(http_message_t *request, http_message_t *response)
{
	int ret = EREJECT;
	http_connector_list_t *connector = request->connector;
	if (connector && connector->func)
	{
		message_dbg("message %p connector \"%s\"", request->client, connector->name);
		ret = connector->func(connector->arg, request, response);
	}
	return ret;
}

int httpmessage_addheader(http_message_t *message, const char *key, const char *value, ssize_t valuelen)
{
	if (key == NULL)
		return EREJECT;
	if ((message->state & GENERATE_MASK) >= GENERATE_SEPARATOR)
	{
		warn("message: result generated, header %s too late", key);
		return EREJECT;
	}
	if (message->headers_storage == NULL)
	{
		message->headers_storage = _buffer_create(str_headerstorage, MAXCHUNKS_HEADER);
	}
	size_t keylen = strlen(key);
	if (value != NULL && valuelen == -1)
		valuelen = strlen(value);
	int ret = 0;
	const string_t mulitdefined[] = {
		STRING_DCL(str_setcookie),
	};
	for (int i = 0 ; i < sizeof(mulitdefined) / sizeof(*mulitdefined); i++)
	{
		if (!_string_cmp(&mulitdefined[i], key, keylen))
			ret = 1;
	}
	if (ret == 0)
	{
		const char *prevkey = strstr(message->headers_storage->data, key);
		if (prevkey != NULL && prevkey[keylen] == ':')
		{
			err("message: header already present %s %.*s", key, (int)valuelen, prevkey + keylen + 1);
			return EREJECT;
		}
	}
	if (_buffer_accept(message->headers_storage, keylen + 2 + valuelen + 2) == ESUCCESS)
	{
		_buffer_append(message->headers_storage, key, keylen);
		_buffer_append(message->headers_storage, ": ", 2);
		if (value != NULL)
			_buffer_append(message->headers_storage, value, valuelen);
		_buffer_append(message->headers_storage, "\r\n", 2);
	}
	else
	{
		err("message: buffer too small to add %s", key);
		return EREJECT;
	}
	return ESUCCESS;
}

int httpmessage_appendheader(http_message_t *message, const char *key, const char *value, ssize_t valuelen)
{
	if (message->headers_storage == NULL)
	{
		message->headers_storage = _buffer_create(str_headerstorage, MAXCHUNKS_HEADER);
	}
	/**
	 * check the key of the current header
	 */
	const char *end = message->headers_storage->offset - 2;
	while (*end != '\n' && end >= _buffer_get(message->headers_storage, 0) ) end--;
	int length = strlen(key);
	if (strncmp(end + 1, key, length))
		return EREJECT;
	/**
	 * remove the ending \r\n of the previous header
	 */
	_buffer_pop(message->headers_storage, 2);
	if (value == NULL)
		return EREJECT;
	if (valuelen == -1)
		valuelen = strlen(value);

	if (_buffer_accept(message->headers_storage, valuelen) == ESUCCESS)
	{
		_buffer_append(message->headers_storage, value, valuelen);
	}
	else
	{
		err("message: headers too long %s", value);
		_buffer_append(message->headers_storage, "\r\n", 2);
		return EREJECT;
	}
	_buffer_append(message->headers_storage, "\r\n", 2);
	return ESUCCESS;
}

int httpmessage_addcontent(http_message_t *message, const char *type, const char *content, int length)
{
	if (message->content == NULL)
	{
		if (type == NULL)
		{
			httpmessage_addheader(message, str_contenttype, STRING_REF("text/plain"));
		}
		else if (strcmp(type, "none"))
		{
			httpmessage_addheader(message, str_contenttype, type, -1);
		}
	}
	if (message->content_storage == NULL)
		message->content_storage = _buffer_create(str_content, MAXCHUNKS_CONTENT);
	if (message->content == NULL && content != NULL)
	{
		_buffer_reset(message->content_storage, 0);
		message->content = message->content_storage;
	}

	if (content != NULL)
	{
		buffer_t *buffer = message->content;
		_buffer_reset(buffer, 0);
		if (length == -1)
			length = strnlen(content, buffer->size - 1);
		length = (buffer->size < length)? buffer->size - 1: length;
		memcpy(buffer->offset, content, length);
		buffer->length += length;
		buffer->offset += length;
		buffer->data[buffer->length] = '\0';
	}

	if (_httpmessage_contentempty(message, 1))
	{
		if (length > -1)
			message->content_length = length;
		else if (message->content != NULL)
			message->content_length = message->content->length;
	}
	if (message->content != NULL && _buffer_get(message->content, 0) != NULL )
	{
		return _buffer_length(message->content);
	}
	return 0;
}

int httpmessage_appendcontent(http_message_t *message, const char *content, int length)
{
	if (message->content == NULL && content != NULL)
	{
		message->content_storage = _buffer_create(str_content, MAXCHUNKS_CONTENT);
		message->content = message->content_storage;
	}

	if (message->content != NULL && content != NULL)
	{
		if (length == -1)
			length = strnlen(content, message->content->size - message->content->length);
		if (!_httpmessage_contentempty(message, 1))
			message->content_length += length;
		if (_buffer_append(message->content, content, length) < 0)
			return EREJECT;
		return message->content->size - _buffer_length(message->content);
	}
	return httpclient_server(message->client)->config->chunksize;
}

int httpmessage_keepalive(http_message_t *message)
{
	message->mode |= HTTPMESSAGE_KEEPALIVE;
	return httpclient_socket(message->client);
}

int httpmessage_lock(http_message_t *message)
{
	message->mode |= HTTPMESSAGE_LOCKED;
	return httpclient_socket(message->client);
}

int httpmessage_isprotected(http_message_t *message)
{
	if (message->method == NULL)
		return -1;
	else
		return ((message->method->properties & MESSAGE_PROTECTED) == MESSAGE_PROTECTED);
}

static char host[NI_MAXHOST] = {0};
static char service[NI_MAXSERV];

const char *httpmessage_SERVER(http_message_t *message, const char *key)
{
	if (message->client == NULL || httpclient_server(message->client) == NULL)
		return NULL;
	const char *value = NULL;

	httpmessage_REQUEST2(message, key, &value);
	return value;
}

const char *httpmessage_REQUEST(http_message_t *message, const char *key)
{
	const char *value = NULL;
	httpmessage_REQUEST2(message, key, &value);
	return value;
}

size_t httpmessage_REQUEST2(http_message_t *message, const char *key, const char **value)
{
	size_t valuelen = 0;
	*value = NULL;
	if (!strcasecmp(key, "uri") && (message->uri != NULL))
	{
		*value = _buffer_get(message->uri, 0);
		valuelen = _buffer_length(message->uri);
		/**
		 * For security all the first '/' (and %2f == /) are removed.
		 * The module may use the URI as a path on the file system,
		 * but an absolute path may be dangerous if the module doesn't
		 * manage correctly the "root" directory.
		 *
		 * /%2f///%2f//etc/password translated to etc/password
		 *
		 */
		/**
		 * the full URI is necessary for authentication and filter
		 *
		while (**value == '/' && **value != '\0') *value++;
		 */
	}
	else if (!strcasecmp(key, "query") && (message->query_storage != NULL))
	{
		if (message->queries)
		{
			_buffer_serializedb(message->query_storage , message->queries, '=', '&');
			dbentry_destroy(message->queries);
			message->queries = NULL;
		}
		*value = _buffer_get(message->query_storage, 0);
		valuelen = _buffer_length(message->query_storage);
	}
	else if (!strcasecmp(key, "scheme"))
	{
		*value = _string_get(&message->client->scheme);
		valuelen = _string_length(&message->client->scheme);
	}
	else if (!strcasecmp(key, "version"))
	{
		valuelen = httpserver_version(message->version, value);
	}
	else if (!strcasecmp(key, "method") && (message->method))
	{
		*value = _string_get(&message->method->key);
		valuelen = _string_length(&message->method->key);
	}
	else if (!strcasecmp(key, "result"))
	{
		int i = 0;
		while (_http_message_result[i] != NULL)
		{
			if (_http_message_result[i]->result == message->result)
			{
				*value = _string_get(&_http_message_result[i]->status);
				valuelen = _string_length(&_http_message_result[i]->status);
				break;
			}
			i++;
		}
	}
	else if (!strcasecmp(key, "content") && (message->content != NULL))
	{
		*value = _buffer_get(message->content, 0);
		valuelen = _buffer_length(message->content);
	}
	else if (!strcasecmp(key, str_contenttype))
	{
		valuelen = dbentry_search(message->headers, key, value);
	}
	else if (!strncasecmp(key, "remote_addr", 11))
	{
		if (message->client == NULL)
			return 0;
		struct sockaddr_storage *sin = &message->client->addr;
		socklen_t len = sizeof(message->client->addr);

		memset(host, 0, NI_MAXHOST);
		valuelen = tcpserver_getname(sin, len, host, NI_MAXHOST, 0);
		if ((ssize_t)valuelen > -1)
			*value = host;
		else
			valuelen = 0;
	}
	else if (!strncasecmp(key, "remote_host", 11))
	{
		if (message->client == NULL)
			return 0;
		struct sockaddr_storage *sin = &message->client->addr;
		socklen_t len = sizeof(message->client->addr);

		memset(host, 0, NI_MAXHOST);
		valuelen = tcpserver_getname(sin, len, host, NI_MAXHOST, 1);
		if ((ssize_t)valuelen > -1)
			*value = host;
		else
			valuelen = 0;
	}
	else if (!strncasecmp(key, "remote_port", 11))
	{
		if (message->client == NULL)
			return 0;
		struct sockaddr_storage *sin = &message->client->addr;
		socklen_t len = sizeof(message->client->addr);

		memset(service, 0, NI_MAXSERV);
		valuelen = tcpserver_getname(sin, len, service, NI_MAXSERV, 2);
		if ((ssize_t)valuelen > -1)
			*value = service;
		else
			return 0;
	}
	else if (!strcasecmp(key, "port"))
	{
		struct sockaddr_storage sin = {0};
		socklen_t len = sizeof(sin);

		memset(service, 0, NI_MAXSERV);
		if (!getsockname(httpclient_socket(message->client), (struct sockaddr *)&sin, &len))
		{
			valuelen = tcpserver_getname(&sin, len, service, NI_MAXSERV, 2);
			if ((ssize_t)valuelen > -1)
				*value = service;
			else
				return 0;
		}
	}
	else if (!strcasecmp(key, "addr"))
	{
		struct sockaddr_storage sin = {0};
		socklen_t len = sizeof(sin);

		memset(host, 0, NI_MAXHOST);
		if (!getsockname(httpclient_socket(message->client), (struct sockaddr *)&sin, &len))
		{
			valuelen = tcpserver_getname(&sin, len, host, NI_MAXHOST, 0);
			if ((ssize_t)valuelen > -1)
				*value = host;
			else
				return 0;
		}
		if (*value != host)
		{
			valuelen = httpserver_INFO2(httpclient_server(message->client), "addr", value);
		}
	}
	else
	{
		valuelen = dbentry_search(message->headers, key, value);
	}
	if (valuelen == (size_t)EREJECT)
	{
		valuelen = httpserver_INFO2(httpclient_server(message->client), key, value);
	}
	return valuelen;
}

size_t httpmessage_parameter(http_message_t *message, const char *key, const char **value)
{
	if (message->queries == NULL && message->query_storage != NULL)
	{
		_buffer_filldb(message->query_storage, &message->queries, '=', '&');
	}
	ssize_t valuelen = 0;
	if (message->queries != NULL)
		valuelen = dbentry_search(message->queries, key, value);
	if (valuelen == EREJECT)
		return 0;
	return (size_t)valuelen;
}

size_t httpmessage_cookie(http_message_t *message, const char *key, const char **cookie)
{
	size_t length = 0;
	dbentry_t *entry = dbentry_get(message->cookies, key);
	if (entry && cookie)
	{
		*cookie = message->cookie_storage->data + entry->value.offset;
		length = entry->value.length;
	}
	return length;
}

const void *httpmessage_SESSION(http_message_t *message, const char *key, void *value, int size)
{
	return httpclient_session(message->client, key, strlen(key), value, size);
}

size_t httpmessage_SESSION2(http_message_t *message, const char *key, void **value)
{
	size_t length = 0;
	http_client_t * client = message->client;
	dbentry_t *entry = httpclient_sessioninfo(client, key);
	if (entry)
	{
		if (value)
			*value = client->session->storage->data + entry->value.offset;
		length = entry->value.length;
	}
	return length;
}

void _httpconnector_add(http_connector_list_t **first,
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
