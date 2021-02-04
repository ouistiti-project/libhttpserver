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
#define _HTTPMESSAGE_
#include "_httpmessage.h"
#include "dbentry.h"

#ifndef HTTPMESSAGE_CHUNKSIZE
#define HTTPMESSAGE_CHUNKSIZE 64
#endif

#define buffer_dbg(...)
#define message_dbg(...)
#define client_dbg(...)
#define server_dbg(...)

extern httpserver_ops_t *httpserver_ops;

struct http_connector_list_s
{
	http_connector_t func;
	void *arg;
	struct http_connector_list_s *next;
	const char *name;
	int priority;
};

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

struct http_message_method_s
{
	const char *key;
	short id;
	short properties;
	const http_message_method_t *next;
};

struct http_server_session_s
{
	dbentry_t *dbfirst;
	buffer_t *storage;
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

const char *httpversion[] =
{
	"HTTP/0.9",
	"HTTP/1.0",
	"HTTP/1.1",
	"HTTP/2",
	NULL,
};

const char str_get[] = "GET";
const char str_post[] = "POST";
const char str_head[] = "HEAD";

static const http_message_method_t default_methods[] = {
	{ .key = str_get, .id = MESSAGE_TYPE_GET, .next = (const http_message_method_t*)&default_methods[1]},
	{ .key = str_post, .id = MESSAGE_TYPE_POST, .properties = MESSAGE_ALLOW_CONTENT, .next = (const http_message_method_t*)&default_methods[2]},
	{ .key = str_head, .id = MESSAGE_TYPE_HEAD, .next = NULL},
#ifdef HTTPCLIENT_FEATURES
	{ .key = NULL, .id = -1, .next = NULL},
#endif
};

#ifndef DEFAULTSCHEME
#define DEFAULTSCHEME
const char str_defaultscheme[] = "http";
#endif
const char str_form_urlencoded[] = "application/x-www-form-urlencoded";
const char str_cookie[] = "Cookie";
const char str_true[] = "true";
const char str_false[] = "false";

static char _httpserver_software[] = "libhttpserver";
char *httpserver_software = _httpserver_software;
/********************************************************************/
#define BUFFERMAX 2048
static int ChunkSize = HTTPMESSAGE_CHUNKSIZE;
/**
 * the chunksize has to be constant during the life of the application.
 * Two ways are available:
 *  - to store the chunksize into each buffer (takes a lot of place).
 *  - to store into a global variable (looks bad).
 */
static buffer_t * _buffer_create(int maxchunks)
{
	buffer_t *buffer = vcalloc(1, sizeof(*buffer));
	if (buffer == NULL)
		return NULL;
	/**
	 * nbchunks is unused here, because is it possible to realloc.
	 * Embeded version may use the nbchunk with special vcalloc.
	 * The idea is to create a pool of chunks into the stack.
	 */
	buffer->data = vcalloc(1, ChunkSize + 1);
	if (buffer->data == NULL)
	{
		free(buffer);
		return NULL;
	}
	buffer->maxchunks = maxchunks;
	buffer->size = ChunkSize + 1;
	buffer->offset = buffer->data;
	return buffer;
}

static char *_buffer_append(buffer_t *buffer, const char *data, int length)
{
	if (length == -1)
		length = strlen(data);
	if (length == 0)
		return buffer->offset;

	if (buffer->data + buffer->size < buffer->offset + length)
	{
		int nbchunks = (length / ChunkSize) + 1;
		if (buffer->maxchunks - nbchunks < 0)
			nbchunks = buffer->maxchunks;
		int chunksize = ChunkSize * nbchunks;

		if (chunksize == 0)
		{
			warn("buffer max chunk: %d", buffer->size / ChunkSize);
			return NULL;
		}

		buffer->maxchunks -= nbchunks;
		if ((buffer->size + chunksize) > BUFFERMAX)
		{
			warn("buffer max: %d / %d", buffer->size + chunksize, BUFFERMAX);
			return NULL;
		}
		char *newptr = vrealloc(buffer->data, buffer->size + chunksize);
		if (newptr == NULL)
		{
			buffer->maxchunks = 0;
			warn("buffer out memory: %d", buffer->size + chunksize);
			return NULL;
		}
		buffer->size += chunksize;
		if (newptr != buffer->data)
		{
			char *offset = buffer->offset;
			buffer->offset = newptr + (offset - buffer->data);
			buffer->data = newptr;
		}

		length = (length > chunksize)? (chunksize - 1): length;
	}
	char *offset = memcpy(buffer->offset, data, length);
	buffer->length += length;
	buffer->offset += length;
	buffer->data[buffer->length] = '\0';
	return offset;
}

static char *_buffer_pop(buffer_t *buffer, int length)
{
	length = (length < buffer->length)? length: buffer->length;
	buffer->length -= length;
	buffer->offset -= length;
	*buffer->offset = '\0';
	return buffer->offset;
}

static void _buffer_shrink(buffer_t *buffer, int reset)
{
	buffer->length -= (buffer->offset - buffer->data);
	while (buffer->length > 0 && (*(buffer->offset) == 0))
	{
		buffer->offset++;
		buffer->length--;
	}
	memcpy(buffer->data, buffer->offset, buffer->length);
	buffer->data[buffer->length] = '\0';
	if (!reset)
		buffer->offset = buffer->data + buffer->length;
	else
		buffer->offset = buffer->data;
}

static void _buffer_reset(buffer_t *buffer)
{
	buffer->offset = buffer->data;
	buffer->length = 0;
}

static int _buffer_rewindto(buffer_t *buffer, char needle)
{
	int ret = EINCOMPLETE;
	while (buffer->offset > buffer->data && *(buffer->offset) != needle)
	{
		buffer->offset--;
		buffer->length--;
	}
	if (*(buffer->offset) == needle)
	{
		*buffer->offset = '\0';
		ret = ESUCCESS;
	}
	return ret;
}

static int _buffer_dbentry(buffer_t *storage, dbentry_t **db, char *key, const char * value)
{
	if (key[0] != 0)
	{
		if (value == NULL)
		{
			value = str_true;
		}
		dbentry_t *entry;
		entry = vcalloc(1, sizeof(dbentry_t));
		if (entry == NULL)
			return -1;
		while (*key == ' ')
			key++;
		entry->key = key;
		entry->value = value;
		entry->next = *db;
		*db = entry;
		buffer_dbg("fill \t%s\t%s", key, value);
	}
	return 0;
}

static int _buffer_filldb(buffer_t *storage, dbentry_t **db, char separator, char fieldsep)
{
	int i;
	char *key = storage->data;
	const char *value = NULL;
	int count = 0;

	for (i = 0; i < storage->length; i++)
	{
		if (storage->data[i] == '\n')
			storage->data[i] = '\0';
		if (storage->data[i] == separator && value == NULL)
		{
			storage->data[i] = '\0';
			value = storage->data + i + 1;
			while (*value == ' ')
				value++;
		}
		else if ((storage->data[i] == '\0') ||
				(storage->data[i] == fieldsep))
		{
			storage->data[i] = '\0';
			if (_buffer_dbentry(storage, db, key, value) < 0)
				return -1;
			else
				count++;
			key = storage->data + i + 1;
			value = NULL;
		}
	}
	if (_buffer_dbentry(storage, db, key, value) < 0)
		return -1;
	else
		count++;
	return count;
}

static int _buffer_empty(buffer_t *buffer)
{
	return (buffer->length <= buffer->offset - buffer->data);
}

static char _buffer_last(buffer_t *buffer)
{
	if (buffer->length > 0)
		return *(buffer->offset - 1);
	else
		return 0;
}

static void _buffer_destroy(buffer_t *buffer)
{
	vfree(buffer->data);
	vfree(buffer);
}

HTTPMESSAGE_DECL const char *dbentry_search(dbentry_t *entry, const char *key)
{
	const char *value = NULL;
	while (entry != NULL)
	{
		if (!strcasecmp(entry->key, key))
		{
			value = entry->value;
			break;
		}
		entry = entry->next;
	}
	if (entry == NULL)
	{
		buffer_dbg("dbentry %s not found", key);
	}
	return value;
}

HTTPMESSAGE_DECL void dbentry_revert(dbentry_t *constentry, char separator, char fieldsep)
{
	dbentry_revert_t *entry = (dbentry_revert_t *)constentry;
	while (entry != NULL)
	{
		int i = 0;
		while ((entry->key[i]) != '\0') i++;
		(entry->key)[i] = separator;

		if (entry->key < entry->value)
		{
			int i = 0;
			while ((entry->value[i]) != '\0') i++;
			if ((fieldsep == '\r' || fieldsep == '\n') && entry->value[i + 1] == '\0')
			{
				(entry->value)[i] = '\r';
				(entry->value)[i + 1] = '\n';
			}
			else
				(entry->value)[i] = fieldsep;
		}
		else
		{
			if ((fieldsep == '\r' || fieldsep == '\n') && entry->key[i + 1] == '\0')
			{
				(entry->key)[i] = '\r';
				(entry->key)[i + 1] = '\n';
			}
		}

		entry = entry->next;
	}
}

HTTPMESSAGE_DECL void dbentry_destroy(dbentry_t *entry)
{
	while (entry)
	{
		dbentry_t *next = entry->next;
		free(entry);
		entry = next;
	}
}
/**********************************************************************
 * http_message
 */
int httpmessage_chunksize()
{
	return ChunkSize;
}

#ifdef HTTPCLIENT_FEATURES

static const httpclient_ops_t *httpclient_ops;

void httpclient_appendops(const httpclient_ops_t *ops)
{
	if (httpclient_ops == NULL)
		httpclient_ops = ops;
	else
	{
		((httpclient_ops_t *)ops)->next = httpclient_ops;
		httpclient_ops = ops;
	}
}

http_message_t * httpmessage_create()
{
	http_message_t *client = _httpmessage_create(NULL, NULL);
	return client;
}

void httpmessage_destroy(http_message_t *message)
{
	_httpmessage_destroy(message);
}

http_client_t *httpmessage_request(http_message_t *message, const char *method, char *url)
{
	http_client_t *client = NULL;
	const http_message_method_t *method_it = &default_methods[0];
	while (method_it != NULL)
	{
		if (!strcmp(method_it->key, method))
			break;
		method_it = method_it->next;
	}
	if (method_it == NULL)
	{
		method_it = &default_methods[3];
		((http_message_method_t *)method_it)->key = method;
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
		const httpclient_ops_t *it_ops = httpclient_ops;
		const httpclient_ops_t *protocol_ops = NULL;
		void *protocol_ctx = NULL;
		while (it_ops != NULL)
		{
			if (!strncmp(url, it_ops->scheme, schemelength))
			{
				protocol_ops = it_ops;
				protocol_ctx = client;
				break;
			}
		}
		if (protocol_ops)
			return NULL;
		int iport = protocol_ops->default_port;

		char *port = strchr(host, ':');
		if (port != NULL && port < pathname)
		{
			*port = 0;
			port++;
			iport = atoi(port);
		}

		httpmessage_addheader(message, "Host", host);
		client = httpclient_create(NULL, protocol_ops, protocol_ctx);
		if (client && client->ops->connect)
		{
			int ret = client->ops->connect(client->opsctx, host, iport);
			if (ret == EREJECT)
			{
				err("client connection error");
				httpclient_destroy(client);
				client = NULL;
			}
		}
		else
			err("client creation error");
		url = pathname;
	}
	if (url)
	{
		int length = strlen(url);
		int nbchunks = (length / ChunkSize) + 1;
		message->uri = _buffer_create(nbchunks);
		_buffer_append(message->uri, url, length);
	}
	return client;
}
#endif

HTTPMESSAGE_DECL http_message_t * _httpmessage_create(http_client_t *client, http_message_t *parent)
{
	http_message_t *message;

	message = vcalloc(1, sizeof(*message));
	if (message)
	{
		message->result = RESULT_200;
		message->client = client;
		message->content_length = (unsigned long long)-1;
		if (parent)
		{
			parent->response = message;

			message->method = parent->method;
			message->client = parent->client;
			message->version = parent->version;
			message->result = parent->result;
		}
	}
	return message;
}

HTTPMESSAGE_DECL void _httpmessage_reset(http_message_t *message)
{
	if (message->uri)
		_buffer_reset(message->uri);
	if (message->content)
		_buffer_reset(message->content);
	if (message->headers_storage)
		_buffer_reset(message->headers_storage);
}

HTTPMESSAGE_DECL void _httpmessage_destroy(http_message_t *message)
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
	dbentry_destroy(message->headers);
	if (message->headers_storage)
		_buffer_destroy(message->headers_storage);
	if (message->query_storage)
		_buffer_destroy(message->query_storage);
	dbentry_destroy(message->queries);
	if (message->cookie_storage)
		_buffer_destroy(message->cookie_storage);
	dbentry_destroy(message->cookies);
	vfree(message);
}

static int _httpmessage_changestate(http_message_t *message, int new)
{
	int mask = PARSE_MASK;
	if (new & GENERATE_MASK)
		mask = GENERATE_MASK;
	message->state = new | (message->state & ~mask);
	return message->state;
}

static int _httpmessage_state(http_message_t *message, int check)
{
	int mask = PARSE_MASK;
	if (check & GENERATE_MASK)
		mask = GENERATE_MASK;
	return ((message->state & mask) == check);
}

static int _httpmessage_contentempty(http_message_t *message, int unset)
{
	if (unset)
		return (message->content_length == ((unsigned long long) -1));
	return (message->content_length == 0);
}

static int _httpmessage_parseinit(http_message_t *message, buffer_t *data)
{
	int next = PARSE_INIT;
	const http_message_method_t *method = message->client->server->methods;
	while (method != NULL)
	{
		int length = strlen(method->key);
		if (!strncasecmp(data->offset, method->key, length) &&
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
		method = method->next;
	}

	if (method == NULL)
	{
		err("parse reject method %s", data->offset);
		data->offset++;
		message->version = message->client->server->config->version;
		message->result = RESULT_405;
		next = PARSE_END;
	}
	return next;
}

static int _httpmessage_parseuri(http_message_t *message, buffer_t *data)
{
	int next = PARSE_URI;
	char *uri = data->offset;
	int length = 0;
	int build_uri = 0;
	while (data->offset < (data->data + data->length) && next == PARSE_URI)
	{
		switch (*data->offset)
		{
#ifndef HTTPMESSAGE_NODOUBLEDOT
			case '.':
				next = PARSE_URIDOUBLEDOT;
			break;
#endif
			case '%':
			{
				next = PARSE_URIENCODED;
			}
			break;
			case '/':
				/**
				 * Remove all double / inside the URI.
				 * This may allow unsecure path with double .
				 * But leave double // for the query part
				 */
				if ((data->offset > uri) &&
					(*(data->offset - 1) == '/') &&
					(message->query == NULL))
				{
					data->offset++;
					continue;
				}
				else
					length++;
				/**
				 * URI must be an absolute path
				 */
				build_uri = 1;
			break;
			case '?':
				/** This message query is not used as it **/
				message->query = data->offset;
				length++;
			break;
			case ' ':
			{
				/**
				 * empty URI is accepted
				 */
				if (message->uri == NULL && length == 0)
					build_uri = 1;
				next = PARSE_VERSION;
			}
			break;
			case '*':
				length++;
				/**
				 * '*' URI is accepted
				 */
				if (message->uri == NULL && length == 0)
					build_uri = 1;
			break;
			case '\r':
			case '\n':
			{
				/**
				 * empty URI is accepted
				 */
				if (message->uri == NULL && length == 0)
					build_uri = 1;
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

	if (build_uri && message->uri == NULL)
	{
		message->uri = _buffer_create(MAXCHUNKS_URI);
	}

	if (length > 0 && (message->uri != NULL))
	{
		uri = _buffer_append(message->uri, uri, length);
		if (uri == NULL)
		{
			message->version = message->client->server->config->version;
#ifdef RESULT_414
			message->result = RESULT_414;
#else
			message->result = RESULT_400;
#endif
			next = PARSE_END;
			err("parse reject uri too long : %s %s", message->uri->data, data->data);
		}
	}
	return next;
}

static int _httpmessage_parseuriencoded(http_message_t *message, buffer_t *data)
{
	int next = PARSE_URIENCODED;
	char *encoded = data->offset;
	if (*encoded == '%')
		encoded ++;
	char decodeval = message->decodeval;
	int i = (decodeval == 0)? 0 : 1;
	next = PARSE_URI;
	for (; i < 2; i++)
	{
		decodeval = decodeval << 4;
		if (*encoded > 0x29 && *encoded < 0x40)
			decodeval += (*encoded - 0x30);
		else if (*encoded > 0x40 && *encoded < 0x47)
			decodeval += (*encoded - 0x41 + 10);
		else if (*encoded > 0x60 && *encoded < 0x67)
			decodeval += (*encoded - 0x61 + 10);
		else if (*encoded == '\0')
		{
			/**
			 * not enought data to read the character
			 */
			decodeval = decodeval >> 4;
			next = PARSE_URIENCODED;
			message->decodeval = decodeval;
			break;
		}
		else
		{
			message->result = RESULT_400;
			next = PARSE_END;
			err("parse reject uri : %s %s", message->uri->data, data->data);
		}
		encoded ++;
	}
	if (next == PARSE_URI)
	{
		/**
		 * The URI must be an absolute path.
		 *
		 * if message->uri == NULL and decodeval == '/'
		 * this is the first / of the URI
		 */
		if (decodeval != '/' && message->uri == NULL)
			message->uri = _buffer_create(MAXCHUNKS_URI);
		if (message->uri != NULL)
			_buffer_append(message->uri, &decodeval, 1);
		message->decodeval = 0;
	}
	data->offset = encoded;
	return next;
}

#ifndef HTTPMESSAGE_NODOUBLEDOT
static int _httpmessage_parseuridoubledot(http_message_t *message, buffer_t *data)
{
	int next = PARSE_URI;
	if (message->uri == NULL)
		return next;
	char *uri = data->offset;
	ssize_t length = 1;
	switch (*(data->offset))
	{
		case '.':
			length = -1;
			data->offset++;
		break;
		case '%':
			next = PARSE_URIENCODED;
			data->offset++;
		case ' ':
		case '\0':
		case '\r':
		case '\n':
			length = 0;
		break;
		default:
			data->offset++;
		break;
	}
	if (length > 0)
	{
		_buffer_append(message->uri, ".", 1);
		_buffer_append(message->uri, uri, length);
	}
	else if (length == -1)
	{
		/// remove the first last '/'
		_buffer_rewindto(message->uri, '/');
		/// remove the first directory
		_buffer_rewindto(message->uri, '/');
		if (_buffer_empty(message->uri))
		{
			_buffer_destroy(message->uri);
			message->uri = NULL;
		}
	}
	return next;
}
#endif

static int _httpmessage_parsestatus(http_message_t *message, buffer_t *data)
{
	int next = PARSE_STATUS;
	int i;
	for (i = HTTP09; i < HTTPVERSIONS; i++)
	{
		int length = strlen(httpversion[i]);
		if (!strncasecmp(data->offset, httpversion[i], length))
		{
			message->version = i;
			data->offset += length;
			break;
		}
	}
	if (i < HTTPVERSIONS)
	{
		/** pass the next space character */
		data->offset++;
		char status[4] = {data->offset[0], data->offset[1], data->offset[2], 0};
		message->result = atoi(status);
		httpmessage_addheader(message, "Status", status);
		data->offset = strchr(data->offset, '\n') + 1;
	}
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
	if (data->offset + 10 > data->data + data->length)
	{
		return next;
	}
	char *version = data->offset;
	int i;
	for (i = HTTP09; i < HTTPVERSIONS; i++)
	{
		int length = strlen(httpversion[i]);
		if (!strncasecmp(version, httpversion[i], length))
		{
			data->offset += length;
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
	if (i == HTTPVERSIONS)
	{
		next = PARSE_END;
		message->result = RESULT_400;
		err("request bad protocol version %s", version);
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
		message->query = strchr(message->uri->data, '?');
		warn("new request %s %s from %p", message->method->key, message->uri->data, message->client);
	}
	else if (message->uri == NULL)
	{
		message->result = RESULT_400;
		next = PARSE_END;
		err("parse reject URI bad formatting: %s", data->data);
	}
	if (message->query != NULL)
	{
		*message->query = '\0';
		message->query++;
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
		message->headers_storage = _buffer_create(MAXCHUNKS_HEADER);
	}

	/* store header line as "<key>:<value>\0" */
	while (data->offset < (data->data + data->length) && next == PARSE_HEADER)
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
				_buffer_append(message->headers_storage, header, length + 1);
				header = data->offset + 1;
				length = 0;
				message->state &= ~PARSE_CONTINUE;
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
		next = PARSE_END;
		message->result = RESULT_400;
		err("request bad header %s", message->headers_storage->data);
	}
	else
	{
		_buffer_shrink(data, 1);
		next = PARSE_PRECONTENT;
		message->state &= ~PARSE_CONTINUE;
	}
	return next;
}

static int _httpmessage_parseprecontent(http_message_t *message, buffer_t *data)
{
	int next = PARSE_PRECONTENT;
	int length = 0;
	if (message->query)
		length = strlen(message->query);

	message->content_packet = 0;
	if ((message->method->properties & MESSAGE_ALLOW_CONTENT) &&
		message->content_type != NULL &&
		!strncasecmp(message->content_type, str_form_urlencoded, sizeof(str_form_urlencoded) - 1))
	{
		next = PARSE_POSTCONTENT;
		message->state &= ~PARSE_CONTINUE;
		length += message->content_length;
	}
	else if (_httpmessage_contentempty(message, 0))
	{
		next = PARSE_END;
		dbg("no content inside request");
	}
	else
	/**
	 * data may contain some first bytes from the content
	 * We need to get out from this function use them by
	 * the connector
	 */
	if (!(message->state & PARSE_CONTINUE))
		message->state |= PARSE_CONTINUE;
	else
	{
		next = PARSE_CONTENT;
		message->state &= ~PARSE_CONTINUE;
	}

	if ((message->query != NULL) &&
		(message->query_storage == NULL))
	{
		int nbchunks = (length / ChunkSize ) + 1;
		message->query_storage = _buffer_create(nbchunks);
		_buffer_append(message->query_storage, message->query, -1);
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
		int length = data->length;
		message_dbg("message: content (%d): %.*s", length, length, data->offset);

		/**
		 * If the Content-Length header is not set,
		 * the parser must continue while the socket is opened
		 */
		if (_httpmessage_contentempty(message, 1))
		{
			length -= (data->offset - data->data);
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
			length -= (data->offset - data->data);
		}

		if (message->content == NULL)
		{
			message->content_storage = _buffer_create(1);
			message->content = message->content_storage;
		}
		_buffer_reset(message->content);
		if (message->content != data)
			_buffer_append(message->content, data->offset, length);
		message->content_packet = length;
		if (!_httpmessage_contentempty(message, 1))
			message->content_length -= length;
		data->offset += length;
	}
	return next;
}

static int _httpmessage_parsepostcontent(http_message_t *message, buffer_t *data)
{
	int next = PARSE_POSTCONTENT;
	char *query = data->offset;
	int length = data->length -(data->offset - data->data);
	/**
	 * message mix query data inside the URI and Content
	 */
	if (message->query_storage == NULL)
	{
		int nbchunks = (data->length / ChunkSize ) + 1;
		message->query_storage = _buffer_create(nbchunks);
	}
	if (message->query != NULL)
	{
		_buffer_append(message->query_storage, "&", 1);
		message->query = NULL;
	}
	while (length > 0 && query[length - 1] == '\n' || query[length - 1] == '\r')
		length--;
	_buffer_append(message->query_storage, query, length);
	if (message->content_length <= length)
	{
		data->offset += message->content_length;
		message->content = message->query_storage;
		message->content_packet = message->query_storage->length;
		message->content_length = message->query_storage->length;
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
HTTPMESSAGE_DECL int _httpmessage_parserequest(http_message_t *message, buffer_t *data)
{
	int ret = ECONTINUE;

	do
	{
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
			case PARSE_URIENCODED:
			{
				next = _httpmessage_parseuriencoded(message, data);
			}
			break;
#ifndef HTTPMESSAGE_NODOUBLEDOT
			case PARSE_URIDOUBLEDOT:
			{
				next = _httpmessage_parseuridoubledot(message, data);
			}
			break;
#endif
			case PARSE_STATUS:
			{
				next = _httpmessage_parsestatus(message, data);
			}
			break;
			case PARSE_VERSION:
			{
				next = _httpmessage_parseversion(message, data);
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
				message_dbg("parse end with %d data: %s", data->length, data->offset);

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

HTTPMESSAGE_DECL int _httpmessage_buildresponse(http_message_t *message, int version, buffer_t *header)
{
	http_message_version_e _version = message->version;
	if (message->version > (version & HTTPVERSION_MASK))
		_version = (version & HTTPVERSION_MASK);
	_buffer_append(header, httpversion[_version], strlen(httpversion[_version]));

	char *status = _httpmessage_status(message);
	_buffer_append(header, status, strlen(status));
	_buffer_append(header, "\r\n", 2);

	header->offset = header->data;
	return ESUCCESS;
}

HTTPMESSAGE_DECL buffer_t *_httpmessage_buildheader(http_message_t *message)
{
	if (message->headers != NULL)
	{
		dbentry_revert(message->headers, ':', '\n');
		dbentry_destroy(message->headers);
		message->headers = NULL;
	}
	if (!_httpmessage_contentempty(message, 1))
	{
		char content_length[32];
		snprintf(content_length, 31, "%llu",  message->content_length);
		httpmessage_addheader(message, str_contentlength, content_length);
		if ((message->mode & HTTPMESSAGE_KEEPALIVE) > 0)
		{
			httpmessage_addheader(message, str_connection, "Keep-Alive");
		}
	}
	else
	{
		message->mode &= ~HTTPMESSAGE_KEEPALIVE;
		httpmessage_addheader(message, str_connection, "Close");
	}
	message->headers_storage->offset = message->headers_storage->data;
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

int httpmessage_content(http_message_t *message, char **data, unsigned long long *content_length)
{
	int size = 0;
	int state = message->state & PARSE_MASK;
	if (content_length != NULL)
	{
		if (!_httpmessage_contentempty(message, 1))
			*content_length = message->content_length;
		else
			*content_length = 0;
	}
	if (message->content)
	{
		size = message->content_packet;
		if (data)
		{
			*data = message->content->data;
		}
	}
	if ((message->state & GENERATE_MASK) != 0)
		return size;
	if (state < PARSE_CONTENT)
		return EINCOMPLETE;
	if (size == 0 && state >= PARSE_CONTENT)
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

HTTPMESSAGE_DECL char *_httpmessage_status(http_message_t *message)
{
	int i = 0;
	while (_http_message_result[i] != NULL)
	{
		if (_http_message_result[i]->result == message->result)
			return _http_message_result[i]->status;
		i++;
	}
	static char status[] = " XXX ";
	snprintf(status, 6, " %.3d", message->result);
	return status;
}

HTTPMESSAGE_DECL int _httpmessage_fillheaderdb(http_message_t *message)
{
	_buffer_filldb(message->headers_storage, &message->headers, ':', '\r');
	const char *value = NULL;
	value = dbentry_search(message->headers, str_connection);
	if (value != NULL && strcasestr(value, "Keep-Alive") != NULL)
		message->mode |= HTTPMESSAGE_KEEPALIVE;
	if (value != NULL && strcasestr(value, "Upgrade") != NULL)
	{
		warn("Connection upgrading");
		message->mode |= HTTPMESSAGE_LOCKED;
	}
	value = dbentry_search(message->headers, str_contentlength);
	if (value != NULL)
		message->content_length = atoi(value);
	value = dbentry_search(message->headers, str_contenttype);
	if (value != NULL)
		message->content_type = value;
	value = dbentry_search(message->headers, "Status");
	if (value != NULL)
		httpmessage_result(message, atoi(value));
	value = dbentry_search(message->headers, str_cookie);
	if (value != NULL)
		message->cookie = value;
	return ESUCCESS;
}

HTTPMESSAGE_DECL int _httpmessage_runconnector(http_message_t *request, http_message_t *response)
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

void httpmessage_addheader(http_message_t *message, const char *key, const char *value)
{
	if (message->headers_storage == NULL)
	{
		message->headers_storage = _buffer_create(MAXCHUNKS_HEADER);
	}
	_buffer_append(message->headers_storage, key, strlen(key));
	_buffer_append(message->headers_storage, ": ", 2);
	_buffer_append(message->headers_storage, value, strlen(value));
	_buffer_append(message->headers_storage, "\r\n", 2);
}

int httpmessage_appendheader(http_message_t *message, const char *key, const char *value, ...)
{
	if (message->headers_storage == NULL)
	{
		message->headers_storage = _buffer_create(MAXCHUNKS_HEADER);
	}
	const char *end = message->headers_storage->offset - 2;
	while (*end != '\n' && end >= message->headers_storage->data ) end--;
	int length = strlen(key);
	if (strncmp(end + 1, key, length))
		return EREJECT;
	_buffer_pop(message->headers_storage, 2);
#ifdef USE_STDARG
	va_list ap;
	va_start(ap, value);
	while (value != NULL)
	{
#endif
		_buffer_append(message->headers_storage, value, strlen(value));
#ifdef USE_STDARG
		value = va_arg(ap, const char *);
	}
	va_end(ap);
#endif
	_buffer_append(message->headers_storage, "\r\n", 2);
	return ESUCCESS;
}

int httpmessage_addcontent(http_message_t *message, const char *type, const char *content, int length)
{
	if (message->content == NULL)
	{
		if (type == NULL)
		{
			httpmessage_addheader(message, str_contenttype, "text/plain");
		}
		else if (strcmp(type, "none"))
		{
			httpmessage_addheader(message, str_contenttype, type);
		}
	}
	if (message->content == NULL && content != NULL)
	{
		message->content_storage = _buffer_create(MAXCHUNKS_CONTENT);
		message->content = message->content_storage;
	}

	if (content != NULL)
	{
		_buffer_reset(message->content);
		if (length == -1)
			length = strlen(content);
		_buffer_append(message->content, content, length);
	}

	if (_httpmessage_contentempty(message, 1))
	{
		message->content_length = length;
	}
	if (message->content != NULL && message->content->data != NULL )
	{
		return message->content->length;
	}
	return 0;
}

int httpmessage_appendcontent(http_message_t *message, const char *content, int length)
{
	if (message->content == NULL && content != NULL)
	{
		message->content_storage = _buffer_create(MAXCHUNKS_CONTENT);
		message->content = message->content_storage;
	}

	if (message->content != NULL && content != NULL)
	{
		if (length == -1)
			length = strlen(content);
		if (!_httpmessage_contentempty(message, 1))
			message->content_length += length;
		_buffer_append(message->content, content, length);
		return message->content->size - message->content->length;
	}
	return message->client->server->config->chunksize;
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
		if (request != client->request)
			_httpmessage_destroy(request);
		request = next;
	}
	client->request_queue = NULL;
	if (client->request)
		_httpmessage_destroy(client->request);
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
	request->state = GENERATE_INIT | (request->state & ~GENERATE_MASK);
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
 * @param prequest the pointer to the request to create and to fill.
 *
 * @return EINCOMPLETE : data is received and parsed, the function needs to be call again to read more data without waiting.
 * ECONTINUE: not enough data for parsing, need to wait more data.
 * ESUCCESS : the request is fully received and parsed. The request may be running.
 */
static int _httpclient_message(http_client_t *client, http_message_t **prequest)
{
	int size;
	/**
	 * here, it is the call to the recvreq callback from the
	 * server configuration.
	 * see http_server_config_t and httpserver_create
	 */
	size = client->client_recv(client->recv_arg, client->sockdata->offset, client->sockdata->size - client->sockdata->length - 1);
	if (size > 0)
	{
		client->sockdata->length += size;
		client->sockdata->offset[size] = 0;

		/**
		 * WAIT_ACCEPT does the first initialization
		 * otherwise the return is EREJECT
		 */
		int timer = WAIT_TIMER * 3;
		if (client->server->config->keepalive)
			timer = client->server->config->keepalive;
		client->timeout = timer * 100;
		size = ESUCCESS;
	}
	else if (size == 0 || size == EINCOMPLETE)
	{
		size = ECONTINUE;
	}

	/**
	 * the message must be create in all cases
	 * the sockdata may contain a new message
	 */
	if (*prequest == NULL)
		*prequest = _httpmessage_create(client, NULL);

	if (client->sockdata->length > 0)
	{
		/**
		 * the buffer must always be read from the beginning
		 */
		client->sockdata->offset = client->sockdata->data;

		int ret = _httpmessage_parserequest(*prequest, client->sockdata);

		switch (ret)
		{
		case ESUCCESS:
			if (((*prequest)->mode & HTTPMESSAGE_KEEPALIVE) &&
				((*prequest)->version > HTTP10))
			{
				dbg("request: set keep-alive");
				client->state |= CLIENT_KEEPALIVE;
				size = ESUCCESS;
			}
		break;
		case ECONTINUE:
		return EINCOMPLETE;
		case EINCOMPLETE:
		return EINCOMPLETE;
		case EREJECT:
		{
			if ((*prequest)->response == NULL)
				(*prequest)->response = _httpmessage_create(client, *prequest);

			/**
			 * The format of the request is bad. It may be an attack.
			 */
			warn("bad request");
			(*prequest)->response->state = PARSE_END | GENERATE_ERROR;
			(*prequest)->state = PARSE_END;
			client->state = CLIENT_EXIT | CLIENT_ERROR;
			// The response is an error and it is ready to be sent
			size = ESUCCESS;
		}
		break;
		default:
			err("client: parserequest error");
		break;
		}
		/**
		 * the request is not fully received.
		 * The request must not run into _httpclient_request
		 */
		if (((*prequest)->state & PARSE_MASK)  < PARSE_END)
			*prequest = NULL;
	}
	return size;
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
	if (((request->state & PARSE_MASK) > PARSE_POSTHEADER) &&
		((request->state & GENERATE_MASK) == 0))
	{
		if (request->response == NULL)
			request->response = _httpmessage_create(client, request);

		/**
		 * this condition is necessary for bad request parsing
		 */
		if ((request->response->state & PARSE_MASK) < PARSE_END)
		{
			/*
			 * the connector should run only on pushed request
			 */
			if (request->connector == NULL)
				ret = _httpclient_checkconnector(client, request, request->response);
			else
				ret = _httpmessage_runconnector(request, request->response);

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
				request->response->state = PARSE_END | GENERATE_INIT | (request->response->state & ~PARSE_MASK);
				if (request->mode & HTTPMESSAGE_LOCKED)
				{
					client->state |= CLIENT_LOCKED;
				}
			}
			break;
			case ECONTINUE:
				if ((request->response->state & PARSE_MASK) < PARSE_POSTHEADER)
					request->response->state = PARSE_POSTHEADER | (request->response->state & ~PARSE_MASK);
				request->response->state = GENERATE_INIT | (request->response->state & ~GENERATE_MASK);
				request->response->state |= PARSE_CONTINUE;
				if ((request->mode & HTTPMESSAGE_LOCKED) ||
					(request->response->mode & HTTPMESSAGE_LOCKED))
				{
					client->state |= CLIENT_LOCKED;
				}
			break;
			case EINCOMPLETE:
			return EINCOMPLETE;
			case EREJECT:
			{
				if (request->response->result == RESULT_200)
					request->response->result = RESULT_404;
				request->response->state = PARSE_END | GENERATE_ERROR | (request->response->state & ~PARSE_MASK);
				// The response is an error and it is ready to be sent
				ret = ESUCCESS;
			}
			break;
			default:
				err("client: connector error");
			break;
			}
		}
		_httpclient_pushrequest(client, request);
	}
	else if ((request->state & GENERATE_MASK) == 0)
		ret = EINCOMPLETE;
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
			ret = ESUCCESS;
		}
	}
	else
	{
		client_dbg("empty buffer to send");
		ret = ESUCCESS;
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
	int ret = ECONTINUE;
	http_message_t *response = request->response;

	if (((response->state & GENERATE_MASK) > GENERATE_SEPARATOR) &&
		(response->state & PARSE_CONTINUE))
	{
		ret = _httpmessage_runconnector(request, response);

		switch (ret)
		{
		case ESUCCESS:
		{
			client->state = CLIENT_WAITING | (client->state & ~CLIENT_MACHINEMASK);
			response->state = PARSE_END | (response->state & ~PARSE_MASK);
			response->state &= ~PARSE_CONTINUE;
			if (response->mode & HTTPMESSAGE_LOCKED)
			{
				client->state |= CLIENT_LOCKED;
			}
		}
		break;
		case EREJECT:
		{
			if (response->result == RESULT_200)
				response->result = RESULT_400;
			response->state = PARSE_END | GENERATE_ERROR | (request->response->state & ~PARSE_MASK);
			response->state &= ~PARSE_CONTINUE;
		}
		break;
		case ECONTINUE:
		{
			response->state |= PARSE_CONTINUE;
			if (response->mode & HTTPMESSAGE_LOCKED)
			{
				client->state |= CLIENT_LOCKED;
			}
		}
		break;
		case EINCOMPLETE:
		{
		}
		break;
		default:
			err("client: connector error");
		break;
		}
	}

	switch (response->state & GENERATE_MASK)
	{
		case GENERATE_ERROR:
		{
			if (response->version == HTTP09)
			{
				response->state = GENERATE_CONTENT | (response->state & ~GENERATE_MASK);
			}
			else
			{
				if (response->header == NULL)
					response->header = _buffer_create(MAXCHUNKS_HEADER);
				buffer_t *buffer = response->header;
				response->state = GENERATE_RESULT | (response->state & ~GENERATE_MASK);
				_httpmessage_buildresponse(response, client->server->config->version, buffer);
			}
			response->state &= ~PARSE_CONTINUE;

		}
		break;
		case GENERATE_INIT:
		{
			if (response->version == HTTP09)
				response->state = GENERATE_CONTENT | (response->state & ~GENERATE_MASK);
			else
			{
				if (response->header == NULL)
					response->header = _buffer_create(MAXCHUNKS_HEADER);
				buffer_t *buffer = response->header;
				if ((request->response->state & PARSE_MASK) >= PARSE_POSTHEADER)
				{
					response->state = GENERATE_RESULT | (response->state & ~GENERATE_MASK);
					_httpmessage_buildresponse(response,response->version, buffer);
				}
			}
			ret = ECONTINUE;
		}
		break;
		case GENERATE_RESULT:
		{
			/**
			 * here, it is the call to the sendresp callback from the
			 * server configuration.
			 * see http_server_config_t and httpserver_create
			 */
			ret = _httpclient_sendpart(client, response->header);
			if (ret != ECONTINUE)
				break;

			/**
			 * for error the content must be set before the header
			 * generation to set the ContentLength
			 */
			if ((response->result >= 299) &&
				(response->content == NULL))
			{
				const char *value = _httpmessage_status(response);
				httpmessage_addcontent(response, "text/plain", value, strlen(value));
				httpmessage_appendcontent(response, "\n\r", 2);
			}

			response->state = GENERATE_HEADER | (response->state & ~GENERATE_MASK);
			_buffer_destroy(response->header);
			response->header = NULL;
			_httpmessage_buildheader(response);
		}
		break;
		case GENERATE_HEADER:
		{
			ret = _httpclient_sendpart(client, response->headers_storage);
			if (ret != ECONTINUE)
				break;
			response->state = GENERATE_SEPARATOR | (response->state & ~GENERATE_MASK);
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
				response->state = GENERATE_END | (response->state & ~GENERATE_MASK);
			else if (response->content != NULL)
			{
				/**
				 * send the first part of the content.
				 * The next loop may append data into the content, but
				 * the first part has to be already sent
				 */
				if (response->content_length != (unsigned long long) -1)
					response->content_length -= response->content->length;
				ret = _httpclient_sendpart(client, response->content);
				if (ret != ECONTINUE || (response->content->length == 0 && !(response->state & PARSE_CONTINUE)))
				{
					response->state = GENERATE_END | (response->state & ~GENERATE_MASK);
				}
				else
					response->state = GENERATE_CONTENT | (response->state & ~GENERATE_MASK);
			}
			else if (response->state & PARSE_CONTINUE)
					response->state = GENERATE_CONTENT | (response->state & ~GENERATE_MASK);
			else
				response->state = GENERATE_END | (response->state & ~GENERATE_MASK);
		}
		break;
		case GENERATE_CONTENT:
		{
			/**
			 * The module may send data by itself (mod_sendfile).
			 * In this case the content doesn't existe but the connector
			 * has to be called
			 */
			if (response->content != NULL)
			{
				static long long sent = 0;
				sent += response->content->length;
				if (response->content_length != (unsigned long long) -1)
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
				ret = _httpclient_sendpart(client, response->content);
				//warn("sent 3 content %d %lld %lld", ret, response->content_length, sent);
				//if ((response->content->length <= 0 ) && (response->state & PARSE_MASK) == PARSE_END)
			}
			if (ret != ECONTINUE || (response->content_length == 0 && !(response->state & PARSE_CONTINUE)))
			{
				response->state = GENERATE_END | (response->state & ~GENERATE_MASK);
				ret = ECONTINUE;
			}
			if (ret != ECONTINUE)
			{
				_buffer_shrink(response->content);
				break;
			}
		}
		break;
		case GENERATE_END:
		{
			http_connector_list_t *callback = request->connector;
			const char *name = "server";
			if (callback)
				name = callback->name;
			warn("response to %p from connector \"%s\" result %d", client, name, request->response->result);
			ret = ESUCCESS;
		}
		break;
		default:
			err("client: bad state %d", response->state & GENERATE_MASK);
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

	/**
	 * The best place to reset the socket buffer. The connector of the current request
	 * is running and may need the content data.
	 * In some cases the buffer needs to be reset after _httpclient_message and in some
	 * othe cases it needs to be reset after _httpmessage_runconnector.
	 */
	if (client->sockdata->length <= (client->sockdata->offset - client->sockdata->data))
	{
		_buffer_reset(client->sockdata);
	}

	switch (client->state & CLIENT_MACHINEMASK)
	{
		case CLIENT_NEW:
			wait_option = WAIT_ACCEPT;
		case CLIENT_WAITING:
		{
			recv_ret = _httpclient_wait(client, wait_option);
			if (recv_ret == ESUCCESS)
			{
				client->state = CLIENT_READING | (client->state & ~CLIENT_MACHINEMASK);
			}
			else if (recv_ret == EREJECT)
			{
				client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
				client->state |= CLIENT_ERROR;
			}
		}
		break;
		case CLIENT_READING:
		{
			if (client->sockdata->offset != client->sockdata->data)
			{
				_buffer_shrink(client->sockdata);
			}
			/**
			 * The modification of the client state is done after
			 * _httpclient_message
			 */
			recv_ret = ESUCCESS;
		}
		break;
		case CLIENT_SENDING:
		{
			send_ret = _httpclient_wait(client, WAIT_SEND);
			if (!(client->state & CLIENT_LOCKED))
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

	if (recv_ret == ESUCCESS)
	{
		int ret = _httpclient_message(client, &client->request);
		if (ret == ECONTINUE)
		{
			client->state = CLIENT_WAITING | (client->state & ~CLIENT_MACHINEMASK);
		}
		else if (ret == EINCOMPLETE)
		{
			client->state = CLIENT_READING | (client->state & ~CLIENT_MACHINEMASK);
		}
		else if (ret == EREJECT)
			client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
	}

	if (client->request != NULL)
	{
		int ret = _httpclient_request(client, client->request);
		//if (ret != EINCOMPLETE && (client->request->state & PARSE_MASK) == PARSE_END)
		if (ret != EINCOMPLETE)
		{
			client->request = NULL;
		}

		if (ret == ESUCCESS || ret == ECONTINUE)
		{
			send_ret = _httpclient_wait(client, WAIT_SEND);
			client->state = CLIENT_SENDING | (client->state & ~CLIENT_MACHINEMASK);
		}
		else if (ret == EREJECT)
			client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
	}

	if ((send_ret == ESUCCESS) && (client->request_queue))
	{
		http_message_t *request = client->request_queue;
		if (request->response == NULL)
		{
			err("internal error: response should be created");
			exit(0);
		}
		else
		{
			int ret = EINCOMPLETE;
			do
			{
				ret = _httpclient_response(client, request);
			} while (ret == EINCOMPLETE);

			if (ret == ESUCCESS)
			{
				ret = ECONTINUE;

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
				else if (httpmessage_result(request->response, -1) > 299)
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
		ChunkSize = config->chunksize;

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
static char host[NI_MAXHOST], service[NI_MAXSERV];
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

const char *httpmessage_SERVER(http_message_t *message, const char *key)
{
	if (message->client == NULL || message->client->server == NULL)
		return NULL;
	const char *value = default_value;

	if (!strcasecmp(key, "port"))
	{
		struct sockaddr_in sin;
		socklen_t len = sizeof(sin);
		if (getsockname(httpclient_socket(message->client), (struct sockaddr *)&sin, &len) == 0)
		{
			getnameinfo((struct sockaddr *) &sin, len,
				0, 0,
				service, NI_MAXSERV, NI_NUMERICSERV);
			value = service;
		}
	}
	else if (!strcasecmp(key, "addr"))
	{
		struct sockaddr_in sin;
		socklen_t len = sizeof(sin);
		if (getsockname(httpclient_socket(message->client), (struct sockaddr *)&sin, &len) == 0)
		{
			int ret = getnameinfo((struct sockaddr *) &sin, len,
				host, NI_MAXHOST,
				0, 0, NI_NUMERICHOST);
			value = host;
		}
		if (value != host)
		{
			value = message->client->server->config->addr;
		}
	}
	else
		value = httpserver_INFO(message->client->server, key);
	return value;
}

const char *httpmessage_REQUEST(http_message_t *message, const char *key)
{
	const char *value = default_value;
	if (!strcasecmp(key, "uri"))
	{
		if (message->uri != NULL)
		{
			value = message->uri->data;
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
			while (*value == '/' && *value != '\0') value++;
			 */
		}
	}
	else if (!strcasecmp(key, "query"))
	{
		if (message->query_storage != NULL)
			value = message->query_storage->data;
	}
	else if (!strcasecmp(key, "scheme"))
	{
		value = message->client->ops->scheme;
	}
	else if (!strcasecmp(key, "version"))
	{
		value = httpversion[(message->version & HTTPVERSION_MASK)];
	}
	else if (!strcasecmp(key, "method") && (message->method))
	{
		value = message->method->key;
	}
	else if (!strcasecmp(key, "result"))
	{
		int i = 0;
		while (_http_message_result[i] != NULL)
		{
			if (_http_message_result[i]->result == message->result)
			{
				value = _http_message_result[i]->status;
				break;
			}
			i++;
		}
	}
	else if (!strcasecmp(key, "content"))
	{
		if (message->content != NULL)
		{
			value = message->content->data;
		}
	}
	else if (!strcasecmp(key, str_contenttype))
	{
		if (message->content_type != NULL)
		{
			value = message->content_type;
		}
		if (value == default_value)
		{
			value = dbentry_search(message->headers, key);
		}
	}
	else if (!strncasecmp(key, "remote_addr", 11))
	{
		if (message->client == NULL)
			return NULL;

		getnameinfo((struct sockaddr *) &message->client->addr, sizeof(message->client->addr),
			host, NI_MAXHOST, 0, 0, NI_NUMERICHOST);
		value = host;
	}
#if defined NETDB_REMOTEINFO
	else if (!strncasecmp(key, "remote_", 7))
	{
		if (message->client == NULL)
			return NULL;
		getnameinfo((struct sockaddr *) &message->client->addr, sizeof(message->client->addr),
			host, NI_MAXHOST,
			service, NI_MAXSERV, NI_NUMERICSERV);

		if (!strcasecmp(key + 7, "host"))
			value = host;
		if (!strcasecmp(key + 7, "port"))
			value = service;
	}
#endif
	else
	{
		value = dbentry_search(message->headers, key);
	}
	return value;
}

const char *httpmessage_parameter(http_message_t *message, const char *key)
{
	if (message->queries == NULL)
	{
		_buffer_filldb(message->query_storage, &message->queries, '=', '&');
	}
	return dbentry_search(message->queries, key);
}

const char *httpmessage_cookie(http_message_t *message, const char *key)
{
	if (message->cookies == NULL)
	{
		if (message->cookie == NULL)
			return NULL;
		int nbchunks = ((strlen(message->cookie) + 1) / ChunkSize) + 1;
		message->cookie_storage = _buffer_create(nbchunks);
		_buffer_append(message->cookie_storage, message->cookie, -1);
		_buffer_filldb(message->cookie_storage, &message->cookies, '=', ';');
	}
	return dbentry_search(message->cookies, key);
}

http_server_session_t *_httpserver_createsession(http_server_t *server, http_client_t *client)
{
	http_server_session_t *session = NULL;
	session = vcalloc(1, sizeof(*session));
	if (session)
		session->storage = _buffer_create(MAXCHUNKS_SESSION);
	return session;
}

const void *httpmessage_SESSION(http_message_t *message, const char *key, void *value)
{
	dbentry_t *sessioninfo = NULL;
	if (message->client == NULL)
		return NULL;

	if (message->client->session)
	{
		sessioninfo = message->client->session->dbfirst;

		while (sessioninfo && strcmp(sessioninfo->key, key))
		{
			sessioninfo = sessioninfo->next;
		}
	}

	if (value != NULL)
	{
		if (!message->client->session)
		{
			message->client->session = _httpserver_createsession(message->client->server, message->client);
			sessioninfo = message->client->session->dbfirst;
		}
		if (!sessioninfo)
		{
			sessioninfo = vcalloc(1, sizeof(*sessioninfo));
			if (sessioninfo == NULL)
				return  NULL;
			sessioninfo->key =
				_buffer_append(message->client->session->storage, key, -1);
			sessioninfo->next = message->client->session->dbfirst;
			message->client->session->dbfirst = sessioninfo;
		}
		if (sessioninfo->value != (char *)value)
		{
			sessioninfo->value = (char *)value;
		}
	}
	else if (sessioninfo == NULL)
	{
		return NULL;
	}
	return (const void *)sessioninfo->value;
}
