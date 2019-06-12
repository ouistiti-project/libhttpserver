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
#include <sys/time.h>
#include <signal.h>
#include <poll.h>
#include <netdb.h>

#include "valloc.h"
#include "vthread.h"
#include "dbentry.h"
#include "log.h"
#include "httpserver.h"
#include "_httpserver.h"
#define _HTTPMESSAGE_
#include "_httpmessage.h"

extern httpserver_ops_t *httpserver_ops;

struct http_connector_list_s
{
	char *vhost;
	http_connector_t func;
	void *arg;
	struct http_connector_list_s *next;
	const char *name;
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
	http_message_method_t *next;
};

struct http_server_session_s
{
	dbentry_t *dbfirst;
	buffer_t *storage;
};

static int _httpclient_run(http_client_t *client);

/********************************************************************/
static http_server_config_t defaultconfig = {
	.addr = NULL,
	.port = 80,
	.maxclients = 10,
	.chunksize = 64,
	.keepalive = 1,
	.version = HTTP10,
};

const char *httpversion[] =
{
	"HTTP/0.9",
	"HTTP/1.0",
	"HTTP/1.1",
	"HTTP/2",
	NULL,
};

const char *str_get = "GET";
const char *str_post = "POST";
const char *str_head = "HEAD";
#ifndef DEFAULTSCHEME
#define DEFAULTSCHEME
const char *str_defaultscheme = "http";
#endif
const char *str_form_urlencoded = "application/x-www-form-urlencoded";

static char _httpserver_software[] = "libhttpserver";
char *httpserver_software = _httpserver_software;
/********************************************************************/
#define BUFFERMAX 2048
static int ChunkSize = 0;
static buffer_t * _buffer_create(int nbchunks, int chunksize)
{
	buffer_t *buffer = vcalloc(1, sizeof(*buffer));
	if (buffer == NULL)
		return NULL;
	/**
	 * nbchunks is unused here, because is it possible to realloc.
	 * Embeded version may use the nbchunk with special vcalloc.
	 * The idea is to create a pool of chunks into the stack.
	 */
	buffer->data = vcalloc(1, chunksize + 1);
	if (buffer->data == NULL)
	{
		free(buffer);
		return NULL;
	}
	/**
	 * the chunksize has to be constant during the life of the application.
	 * Two ways are available:
	 *  - to store the chunksize into each buffer (takes a lot of place).
	 *  - to store into a global variable (looks bad).
	 */
	if (ChunkSize == 0)
		ChunkSize = chunksize;
	buffer->maxchunks = nbchunks;
	buffer->size = chunksize + 1;
	buffer->offset = buffer->data;
	return buffer;
}

static char *_buffer_append(buffer_t *buffer, const char *data, int length)
{
	if (buffer->data + buffer->size < buffer->offset + length + 1)
	{
		char *data = buffer->data;
		int nbchunks = length / ChunkSize + 1;
		if (buffer->maxchunks - nbchunks < 0)
			nbchunks = buffer->maxchunks;
		int chunksize = ChunkSize * nbchunks;
		if (nbchunks > 0)
		{
			buffer->maxchunks -= nbchunks;
			data = vrealloc(buffer->data, buffer->size + chunksize);
			if ((data == NULL && errno == ENOMEM) || (buffer->size + chunksize) > BUFFERMAX)
			{
				buffer->maxchunks = 0;
				warn("buffer max: %d / %d", buffer->size + chunksize, BUFFERMAX);
				return NULL;
			}
			buffer->size += chunksize;
			if (data != buffer->data)
			{
				char *offset = buffer->offset;
				buffer->offset = data + (offset - buffer->data);
				buffer->data = data;
			}
		}
		else
			length = 0;
		length = (length > chunksize)? chunksize: length;
	}
	char *offset = buffer->offset;
	if (length > 0)
	{
		memcpy(buffer->offset, data, length);
		buffer->length += length;
		buffer->offset += length;
		*buffer->offset = '\0';
		return offset;
	}
	else
		return NULL;
}

static void _buffer_shrink(buffer_t *buffer)
{
	buffer->length -= (buffer->offset - buffer->data);
	while (buffer->length > 0 && (*(buffer->offset) == 0))
	{
		buffer->offset++;
		buffer->length--;
	}
	memcpy(buffer->data, buffer->offset, buffer->length);
	buffer->data[buffer->length] = '\0';
	buffer->offset = buffer->data + buffer->length;
}

static void _buffer_reset(buffer_t *buffer)
{
	buffer->offset = buffer->data;
	buffer->length = 0;
}

static void _buffer_destroy(buffer_t *buffer)
{
	vfree(buffer->data);
	vfree(buffer);
}

/**********************************************************************
 * http_message
 */
#ifdef HTTPCLIENT_FEATURES
http_message_t * httpmessage_create(int chunksize)
{
	http_message_t *client = _httpmessage_create(NULL, NULL, chunksize);
	return client;
}

void httpmessage_destroy(http_message_t *message)
{
	if (message-> method)
		free(message->method);
	_httpmessage_destroy(message);
}

void httpmessage_request(http_message_t *message, const char *method, char *resource)
{
	message->method = vcalloc(1, sizeof(*message->method));
	if (message->method == NULL)
		return;
	message->method->key = method;
	message->version = HTTP11;
	if (resource)
	{
		int length = strlen(resource);
		int nbchunks = (length / message->chunksize) + 1;
		message->uri = _buffer_create(nbchunks, message->chunksize);
		_buffer_append(message->uri, resource, length);
	}
}
#endif

HTTPMESSAGE_DECL http_message_t * _httpmessage_create(http_client_t *client, http_message_t *parent, int chunksize)
{
	http_message_t *message;

	message = vcalloc(1, sizeof(*message));
	if (message)
	{
		message->result = RESULT_200;
		message->client = client;
		message->chunksize = chunksize;
//		message->content_length = (unsigned long long)-1);
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
		_httpmessage_destroy(message->response);
	if (message->uri)
		_buffer_destroy(message->uri);
	if (message->content)
		_buffer_destroy(message->content);
	if (message->header)
		_buffer_destroy(message->header);
	if (message->headers_storage)
		_buffer_destroy(message->headers_storage);
	if (message->query_storage)
		_buffer_destroy(message->query_storage);
	dbentry_t *header = message->headers;
	while (header)
	{
		dbentry_t *next = header->next;
		free(header);
		header = next;
	}
	vfree(message);
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
				http_message_method_t *method = message->client->server->methods;
				while (method != NULL)
				{
					int length = strlen(method->key);
					if (!strncasecmp(data->offset, method->key, length) &&
						data->offset[length] == ' ')
					{
						message->method = method;
						data->offset += length + 1;
						next = PARSE_URI;
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
			}
			break;
			case PARSE_URI:
			{
				char *uri = data->offset;
				int length = 0;
				if (message->uri == NULL)
				{
					/**
					 * to use parse_cgi from a module, the functions
					 * has to run on message without client attached.
					 */
					message->uri = _buffer_create(MAXCHUNKS_URI, message->chunksize);
				}
				while (data->offset < (data->data + data->length) && next == PARSE_URI)
				{
					switch (*data->offset)
					{
						case ' ':
						{
							next = PARSE_VERSION;
						}
						break;
						case '?':
						{
							length++;
							message->query = message->uri->data + length;
						}
						break;
						case '\r':
						case '\n':
						{
							next = PARSE_HEADER;
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

				if (length > 0)
				{
					uri = _buffer_append(message->uri, uri, length);
					if (uri == NULL && message->query == NULL)
					{
						message->version = message->client->server->config->version;
#ifdef RESULT_414
						message->result = RESULT_414;
#else
						message->result = RESULT_400;
#endif
						next = PARSE_END;
						err("parse reject uri too long 2: %s %s", message->uri->data, data->data);
					}
				}
				if (next != PARSE_URI)
				{
					if (message->uri->length > 0)
					{
						if (message->query == NULL)
							message->query = message->uri->data + message->uri->length;
						warn("new request %s %s from %p", message->method->key, message->uri->data, message->client);
					}
					else
					{
						message->version = message->client->server->config->version;
						message->result = RESULT_400;
						next = PARSE_END;
						err("parse reject uri too short");
					}
				}
			}
			break;
			case PARSE_STATUS:
			{
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
			}
			break;
			case PARSE_VERSION:
			{
				/**
				 * There is not enougth data to parse the version.
				 * Move the rest to the beginning and request
				 * more data.
				 **/
				if (data->offset + 10 > data->data + data->length)
				{
					//_buffer_shrink(data);
					break;
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
							next = PARSE_HEADER;
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
			}
			break;
			case PARSE_HEADER:
			{
				char *header = data->offset;
				int length = 0;
				if (message->headers_storage == NULL)
				{
					message->headers_storage = _buffer_create(MAXCHUNKS_HEADER, message->chunksize);
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
			}
			break;
			case PARSE_POSTHEADER:
			{
				if (_httpmessage_fillheaderdb(message) != ESUCCESS)
				{
					next = PARSE_END;
					message->result = RESULT_400;
					err("request bad header %s", message->headers_storage->data);
				}
//				else if (!(message->state & PARSE_CONTINUE))
//					message->state |= PARSE_CONTINUE;
				else
				{
					next = PARSE_PRECONTENT;
					message->state &= ~PARSE_CONTINUE;
				}
			}
			break;
			case PARSE_PRECONTENT:
			{
				int length = 0;
				if (message->query)
					length = strlen(message->query);

				if (message->method->id == MESSAGE_TYPE_POST &&
					message->content_type != NULL &&
					!strcmp(message->content_type, str_form_urlencoded))
				{
					next = PARSE_POSTCONTENT;
					message->state &= ~PARSE_CONTINUE;
					length += message->content_length;
				}
				else if (message->content_length == 0)
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

				if (message->query_storage == NULL)
				{
					int nbchunks = (length / message->chunksize ) + 1;
					message->query_storage = _buffer_create(nbchunks, message->chunksize);
					if (message->query != NULL)
					{
						_buffer_append(message->query_storage, message->query, length);
						_buffer_append(message->query_storage, "&", 1);
					}
				}
			}
			break;
			case PARSE_CONTENT:
			{
				if (message->content_length == 0)
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
					int length = data->length -(data->offset - data->data);
					//int length = data->length;
					message->content = data;
					/**
					 * At the end of the parsing the content_length of request
					 * is zero. But it is false, the true value is
					 * Sum(content->length);
					 */
					if (message->content_length <= length)
					{
						data->offset += message->content_length;
						message->content_length = 0;
						next = PARSE_END;
					}
					else
					{
						data->offset += length;
						message->content_length -= length;
					}
				}
			}
			break;
			case PARSE_POSTCONTENT:
			{
				char *query = data->offset;
				int length = data->length -(data->offset - data->data);
				_buffer_append(message->query_storage, query, length);
				if (message->content_length <= length)
				{
					data->offset += message->content_length;
					message->content_length = 0;
					next = PARSE_END;
				}
				else
				{
					data->offset += length;
					message->content_length -= length;
					message->state |= PARSE_CONTINUE;
				}
			}
			break;
			case PARSE_END:
			{
				if (message->result == RESULT_200)
					ret = ESUCCESS;
				else
					ret = EREJECT;
			}
			break;
		}
		if (next == (message->state & PARSE_MASK) && (ret == ECONTINUE))
		{
			if (next < PARSE_PRECONTENT)
				ret = EINCOMPLETE;
			break; // get out of the while (ret == ECONTINUE) loop
		}
		message->state = (message->state & ~PARSE_MASK) | next;
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

HTTPMESSAGE_DECL int _httpmessage_buildheader(http_message_t *message, buffer_t *header)
{
	if (message->headers == NULL)
		_httpmessage_fillheaderdb(message);
	dbentry_t *headers = message->headers;
	while (headers != NULL)
	{
		if (!strcmp(headers->key, str_contentlength))
		{
			headers = headers->next;
			continue;
		}
		_buffer_append(header, headers->key, strlen(headers->key));
		_buffer_append(header, ": ", 2);
		_buffer_append(header, headers->value, strlen(headers->value));
		_buffer_append(header, "\r\n", 2);
		headers = headers->next;
	}
	if (message->content_length != (unsigned long long)-1)
	{
		if ((message->mode & HTTPMESSAGE_KEEPALIVE) > 0)
		{
			char keepalive[32];
			snprintf(keepalive, 31, "%s: %s\r\n", str_connection, "Keep-Alive");
			dbg("header %s => %s", str_connection, "Keep-Alive");
			_buffer_append(header, keepalive, strlen(keepalive));
		}
		char content_length[32];
		snprintf(content_length, 31, "%s: %llu\r\n", str_contentlength, message->content_length);
		dbg("header %s => %llu", str_contentlength, message->content_length);
		_buffer_append(header, content_length, strlen(content_length));
	}
	header->offset = header->data;
	return ESUCCESS;
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
	if (message->content)
	{
		if (data)
			*data = message->content->data;
		size = message->content->length;
	}
	if (content_length)
	{
		if (message->content_length <= 0)
			*content_length = 0;
		else
			*content_length = message->content_length;
	}
	if (state < PARSE_CONTENT)
		return EINCOMPLETE;
	if (size == 0 && state > PARSE_CONTENT)
		return EREJECT;
	return size;
}

/**
 * @brief this function is symetric of "parserequest" to read a response
 */
int httpmessage_parsecgi(http_message_t *message, char *data, int *size)
{
	if (data == NULL)
	{
		message->content = NULL;
		if ((message->state & PARSE_MASK) == PARSE_END)
			return ESUCCESS;
		else
			return EINCOMPLETE;
	}
	static buffer_t tempo;
	tempo.data = data;
	tempo.offset = data;
	tempo.length = *size;
	tempo.size = *size;
	if ((message->state & PARSE_MASK) == PARSE_INIT)
		message->state = PARSE_STATUS;
	if (message->content_length == 0)
		message->content_length = (unsigned long long)-1;

	int ret = _httpmessage_parserequest(message, &tempo);
	*size = tempo.length - (tempo.offset - tempo.data);
	if (*size > 0)
		_buffer_shrink(&tempo);

	if ((message->state & PARSE_MASK) == PARSE_END)
	{
		if (*size > 0)
		{
			*size = 0;
		}
		/**
		 * The request may not contain the ContentLength.
		 * In this case the function must continue after the end.
		 * The caller must stop by itself
		 */
		if (message->content_length == (unsigned long long)-1)
			ret = ECONTINUE;
		else
			message->content = NULL;
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
	return NULL;
}

HTTPMESSAGE_DECL int _httpmessage_fillheaderdb(http_message_t *message)
{
	int i;
	buffer_t *storage = message->headers_storage;
	if (storage == NULL)
		return ESUCCESS;
	char *key = storage->data;
	char *value = NULL;

	for (i = 0; i < storage->length; i++)
	{
		if (storage->data[i] == ':' && value == NULL)
		{
			storage->data[i] = '\0';
			value = storage->data + i + 1;
			while (*value == ' ')
				value++;
		}
		else if (storage->data[i] == '\0')
		{
			if (value == NULL)
			{
				dbg("header key %s", key);
				return EREJECT;
			}
			if (key[0] != 0)
				_httpmessage_addheader(message, key, value);
			key = storage->data + i + 1;
			value = NULL;
		}
	}
	return ESUCCESS;
}

HTTPMESSAGE_DECL int _httpmessage_runconnector(http_message_t *request, http_message_t *response)
{
	int ret = EREJECT;
	http_connector_list_t *connector = request->connector;
	if (connector && connector->func)
	{
		ret = connector->func(connector->arg, request, response);
	}
	return ret;
}

void httpmessage_addheader(http_message_t *message, const char *key, const char *value)
{
	if (message->headers_storage == NULL)
	{
		message->headers_storage = _buffer_create(MAXCHUNKS_HEADER, message->chunksize);
	}
	_buffer_append(message->headers_storage, key, strlen(key));
	_buffer_append(message->headers_storage, ":", 1);
	_buffer_append(message->headers_storage, value, strlen(value) + 1);
}

HTTPMESSAGE_DECL void _httpmessage_addheader(http_message_t *message, char *key, char *value)
{
	dbentry_t *headerinfo;
	headerinfo = vcalloc(1, sizeof(dbentry_t));
	if (headerinfo == NULL)
		return;
	headerinfo->key = key;
	headerinfo->value = value;
	headerinfo->next = message->headers;
	message->headers = headerinfo;
	dbg("header %s => %s", key, value);
	if (value)
	{
		if (!strncasecmp(key, str_connection, 10))
		{
			if (strcasestr(value, "Keep-Alive"))
				message->mode |= HTTPMESSAGE_KEEPALIVE;
		}
		if (!strncasecmp(key, str_contentlength, 14))
		{
			message->content_length = atoi(value);
		}
		if (!strncasecmp(key, str_contenttype, 12))
		{
			message->content_type = value;
		}
		if (!strncasecmp(key, "Status", 6))
		{
			int result;
			sscanf(value,"%d",&result);
			httpmessage_result(message, result);
		}
	}
}

int httpmessage_addcontent(http_message_t *message, const char *type, char *content, int length)
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
		message->content = _buffer_create(MAXCHUNKS_CONTENT, message->client->server->config->chunksize);
	}

	if (content != NULL)
	{
		if (length == -1)
			length = strlen(content);
		_buffer_append(message->content, content, length);
	}
	if (message->content_length <= 0)
	{
		message->content_length = length;
	}
	if (message->content != NULL && message->content->data != NULL )
		return message->content->size - message->content->length;
	return message->client->server->config->chunksize;
}

int httpmessage_appendcontent(http_message_t *message, char *content, int length)
{
	if (message->content == NULL && content != NULL)
	{
		message->content = _buffer_create(MAXCHUNKS_CONTENT, message->client->server->config->chunksize);
	}

	if (message->content != NULL && content != NULL)
	{
		if (length == -1)
			length = strlen(content);
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
		return message->method->properties;
}
/***********************************************************************
 * http_client
 */
static void _httpclient_destroy(http_client_t *client);

http_client_t *httpclient_create(http_server_t *server, httpclient_ops_t *fops, int chunksize)
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
			httpclient_addconnector(client, callback->vhost, callback->func, callback->arg, callback->name);
			callback = callback->next;
		}
	}
	memcpy(&client->ops, fops, sizeof(client->ops));
	client->ctx = client;
	client->sockdata = _buffer_create(1, chunksize);
	if (client->sockdata == NULL)
	{
		_httpclient_destroy(client);
		client = NULL;
	}

	return client;
}

static void _httpclient_destroy(http_client_t *client)
{
	client->ops.destroy(client);

	client->modctx = NULL;
	http_connector_list_t *callback = client->callbacks;
	while (callback != NULL)
	{
		http_connector_list_t *next = callback->next;
		if (callback->vhost)
			free(callback->vhost);
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

void httpclient_addconnector(http_client_t *client, char *vhost, http_connector_t func, void *funcarg, const char *name)
{
	http_connector_list_t *callback;

	callback = vcalloc(1, sizeof(*callback));
	if (callback == NULL)
		return;
	if (vhost)
	{
		int length = strlen(vhost);
		callback->vhost = malloc(length + 1);
		strcpy(callback->vhost, vhost);
	}

	callback->func = func;
	callback->name = name;
	callback->arg = funcarg;
	callback->next = client->callbacks;
	client->callbacks = callback;
}

void *httpclient_context(http_client_t *client)
{
	return client->ctx;
}

http_recv_t httpclient_addreceiver(http_client_t *client, http_recv_t func, void *arg)
{
	http_recv_t previous = client->ops.recvreq;
	if (func)
	{
		client->ops.recvreq = func;
		client->ctx = arg;
	}
	return previous;
}

http_send_t httpclient_addsender(http_client_t *client, http_send_t func, void *arg)
{
	http_send_t previous = client->ops.sendresp;
	if (func)
	{
		client->ops.sendresp = func;
		client->ctx = arg;
	}
	return previous;
}

#ifdef HTTPCLIENT_FEATURES
int httpclient_connect(http_client_t *client, char *addr, int port)
{
	if (client->ops.connect)
		return client->ops.connect(client, addr, port);
	return EREJECT;
}

int httpclient_sendrequest(http_client_t *client, http_message_t *request, http_message_t *response)
{
	int size = 0;
	buffer_t *data = _buffer_create(MAXCHUNKS_HEADER, request->chunksize);

	request->client = client;
	response->client = client;
	httpmessage_addheader(request, "Host", httpmessage_REQUEST(request, "remote_host"));
	const char *method = httpmessage_REQUEST(request, "method");
	_buffer_append(data, method, strlen(method));
	_buffer_append(data, " ", 1);
	const char *uri = httpmessage_REQUEST(request, "uri");
	_buffer_append(data, uri, strlen(uri));
	const char *version = httpmessage_REQUEST(request, "version");
	if (version)
	{
		_buffer_append(data, " ", 1);
		_buffer_append(data, version, strlen(version));
	}
	_buffer_append(data, "\r\n", 2);
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
			dbg("send %s", data->offset);
			size = client->ops.sendresp(client->ctx, data->offset, data->length);
		}
		if (size == EINCOMPLETE)
			continue;
		if (size < 0)
			break;
		data->offset += size;
		data->length -= size;
	}
	_buffer_reset(data);
	*(data->offset) = 0;
	_httpmessage_buildheader(request, data);
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
			size = client->ops.sendresp(client->ctx, data->offset, data->length);
		}
		if (size == EINCOMPLETE)
			continue;
		if (size < 0)
			break;
		data->offset += size;
		data->length -= size;
	}

	int ret = ECONTINUE;
	while (ret == ECONTINUE)
	{
		_buffer_reset(data);
		size = client->ops.recvreq(client->ctx, data->offset, data->size - 1);
		if (size >= 0)
		{
			data->length += size;
			data->data[data->length] = 0;
		}
		else if (size == EINCOMPLETE)
			continue;
		else
			break;
		data->offset = data->data;
		if (response->state == PARSE_INIT)
			response->state = PARSE_STATUS;
		ret = _httpmessage_parserequest(response, data);
	}
	_buffer_destroy(data);
	return ESUCCESS;
}
#endif

#ifdef VTHREAD
static int _httpclient_connect(http_client_t *client)
{
	int ret;
	client->state &= ~CLIENT_STARTED;
	client->state |= CLIENT_RUNNING;
#ifndef SHARED_SOCKET
	/*
	 * TODO : dispatch close and destroy from tcpserver.
	 */
	close(client->server->sock);
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

static int _httpclient_checkconnector(http_client_t *client, http_message_t *request, http_message_t *response)
{
	int ret = ESUCCESS;
	char *vhost = NULL;
	http_connector_list_t *iterator = request->connector;

	iterator = client->callbacks;
	while (iterator != NULL)
	{
		vhost = iterator->vhost;
		if (vhost != NULL)
		{
			const char *host = httpmessage_REQUEST(request, "Host");
			if (!strcasecmp(vhost, host))
				vhost = NULL;
		}

		if (vhost == NULL && iterator->func)
		{
			ret = iterator->func(iterator->arg, request, response);
			if (ret != EREJECT)
			{
				if (ret == ESUCCESS)
				{
					client->state |= CLIENT_RESPONSEREADY;
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
	size = client->ops.recvreq(client->ctx, client->sockdata->offset, client->sockdata->size - client->sockdata->length - 1);
	if (size > 0)
	{
		client->sockdata->length += size;
		client->sockdata->offset[size] = 0;
		client->sockdata->offset = client->sockdata->data;

		if (*prequest == NULL)
			*prequest = _httpmessage_create(client, NULL, client->server->config->chunksize);

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
	if (client->sockdata->length > 0)
	{
		int ret = _httpmessage_parserequest(*prequest, client->sockdata);

		switch (ret)
		{
		case ESUCCESS:
			if (((*prequest)->mode & HTTPMESSAGE_KEEPALIVE) &&
				((*prequest)->version > HTTP10))
			{
				client->state |= CLIENT_KEEPALIVE;
				size = ESUCCESS;
			}
		break;
		case ECONTINUE:
			return EINCOMPLETE;
		break;
		case EINCOMPLETE:
			return EINCOMPLETE;
		break;
		case EREJECT:
		{
			if ((*prequest)->response == NULL)
				(*prequest)->response = _httpmessage_create(client, *prequest, client->server->config->chunksize);

			warn("bad resquest");
			(*prequest)->response->state = PARSE_END | GENERATE_ERROR;
			(*prequest)->state = PARSE_END;
			// The response is an error and it is ready to be sent
			size = ESUCCESS;
		}
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
			request->response = _httpmessage_create(client, request, client->server->config->chunksize);

		/**
		 * this condition is necessary for bad request parsing
		 */
		if ((request->response->state & PARSE_MASK) < PARSE_END)
		{
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
				client->state = CLIENT_WAITING | (client->state & CLIENT_MACHINEMASK);
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
			break;
			case EINCOMPLETE:
				return EINCOMPLETE;
			break;
			case EREJECT:
			{
				if (request->response->result == RESULT_200)
					request->response->result = RESULT_404;
				request->response->state = PARSE_END | GENERATE_ERROR | (request->response->state & ~PARSE_MASK);
				// The response is an error and it is ready to be sent
				ret = ESUCCESS;
			}
			break;
			}
		}
		_httpclient_pushrequest(client, request);
	}
	else if ((request->state & GENERATE_MASK) == 0)
		ret = EINCOMPLETE;
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
			client->state = CLIENT_WAITING | (client->state & CLIENT_MACHINEMASK);
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
		}
		break;
		case EINCOMPLETE:
		{
		}
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
					response->header = _buffer_create(MAXCHUNKS_HEADER, client->server->config->chunksize);
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
					response->header = _buffer_create(MAXCHUNKS_HEADER, client->server->config->chunksize);
				buffer_t *buffer = response->header;
				if ((request->response->state & PARSE_MASK) >= PARSE_POSTHEADER)
				{
					response->state = GENERATE_RESULT | (response->state & ~GENERATE_MASK);
					_httpmessage_buildresponse(response, client->server->config->version, buffer);
				}
			}
			ret = ECONTINUE;
		}
		break;
		case GENERATE_RESULT:
		{
			int size;
			/**
			 * here, it is the call to the sendresp callback from the
			 * server configuration.
			 * see http_server_config_t and httpserver_create
			 */
			buffer_t *buffer = response->header;
			size = client->ops.sendresp(client->ctx, buffer->offset, buffer->length);
			if (size < 0)
			{
				err("client %p RESULT send error %s", client, strerror(errno));
				ret = EREJECT;
				break;
			}
			buffer->offset += size;
			buffer->length -= size;

			/**
			 * for error the content must be set before the header
			 * generation to set the ContentLength
			 */
			if ((response->result > 399) &&
				(response->content == NULL))
			{
				const char *value = _httpmessage_status(response);
				httpmessage_addcontent(response, "text/plain", (char *)value, strlen(value));
				httpmessage_appendcontent(response, (char *)"\n\r", 2);
			}

			if (buffer->length <= 0)
			{
				response->state = GENERATE_HEADER | (response->state & ~GENERATE_MASK);
				_buffer_reset(buffer);
				_httpmessage_buildheader(response, buffer);
			}
			ret = ECONTINUE;
		}
		break;
		case GENERATE_HEADER:
		{
			int size;
			buffer_t *buffer = response->header;
			size = client->ops.sendresp(client->ctx, buffer->offset, buffer->length);
			if (size < 0)
			{
				err("client %p HEADER send error %s", client, strerror(errno));
				ret = EREJECT;
				break;
			}
			buffer->offset += size;
			buffer->length -= size;

			if (buffer->length <= 0)
			{
				response->state = GENERATE_SEPARATOR | (response->state & ~GENERATE_MASK);
			}
			ret = ECONTINUE;
		}
		break;
		case GENERATE_SEPARATOR:
		{
			int size;
			size = client->ops.sendresp(client->ctx, "\r\n", 2);
			if (size < 0)
			{
				err("client %p SEPARATOR send error %s", client, strerror(errno));
				ret = EREJECT;
				break;
			}
			if (request->method && request->method->id == MESSAGE_TYPE_HEAD)
				response->state = GENERATE_END | (response->state & ~GENERATE_MASK);
			else
			{
				response->state = GENERATE_CONTENT | (response->state & ~GENERATE_MASK);
			}
			client->ops.flush(client);
			ret = ECONTINUE;
		}
		break;
		case GENERATE_CONTENT:
		{
			int size;
			buffer_t *buffer = response->content;
			if ((response->state & PARSE_MASK) == PARSE_END)
				response->state = GENERATE_END | (response->state & ~GENERATE_MASK);
			if ((buffer != NULL) && (buffer->length > 0))
			{
				ret = ECONTINUE;
				buffer->offset = buffer->data;
				while (buffer->length > 0)
				{
					size = client->ops.sendresp(client->ctx, buffer->offset, buffer->length);
					if (size == EINCOMPLETE)
					{
						ret = EINCOMPLETE;
						break;
					}
					else if (size < 0)
					{
						err("client %p CONTENT rest %d send error %s", client, buffer->length, strerror(errno));
						ret = EREJECT;
						break;
					}
					buffer->length -= size;
					buffer->offset += size;
				}
				_buffer_reset(buffer);
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
	int ret = ESUCCESS;

#if defined(VTHREAD) && !defined(BLOCK_SOCKET)
	struct timespec *ptimeout = NULL;
	struct timespec timeout;
	int ttimeout = -1;
	if (options & WAIT_SEND)
	{
		timeout.tv_sec = 0;
		timeout.tv_nsec = 10000000;
		ptimeout = &timeout;
		ptimeout = NULL;
		ttimeout = 10;
	}
	else
	{
		timeout.tv_sec = WAIT_TIMER;
		timeout.tv_nsec = 0;
		ptimeout = &timeout;
		ttimeout = WAIT_TIMER * 1000;
	}

	fd_set fds;
	FD_ZERO(&fds);
#ifdef USE_POLL
	struct pollfd poll_set[1];
	int numfds = 0;
	poll_set[0].fd = client->sock;
	if (options & WAIT_SEND)
		poll_set[0].events = POLLOUT;
	else
		poll_set[0].events = POLLIN;
	numfds++;

	/**
	 * ppoll receives SIGCHLD with pthread and fork VTREAD_TYPE.
	 * Normally it shouldn't occure, the client is the child of the server
	 * and has no child (exception for cgi module answer
	 */
	sigset_t sigmask;
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGCHLD);
	//ret = ppoll(poll_set, numfds, ptimeout, NULL);
	ret = poll(poll_set, numfds, ttimeout);
	if (poll_set[0].revents & POLLIN)
	{
		FD_SET(client->sock, &fds);
	}
	else if (poll_set[0].revents & POLLOUT)
	{
		FD_SET(client->sock, &fds);
	}
	else if (ret < 0)
		err("client %p poll %x", client, poll_set[0].revents);
#else
	fd_set *rfds = NULL, *wfds = NULL;
	FD_SET(client->sock, &fds);
	if (options & WAIT_SEND)
		wfds = &fds;
	else
		rfds = &fds;

	sigset_t sigmask;
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGCHLD);
	ret = pselect(client->sock + 1, rfds, wfds, NULL, ptimeout, &sigmask);
#endif

	if (ret == 0)
	{
		if (options & WAIT_ACCEPT)
		{
			ret = EREJECT;
		}
		else if (options & WAIT_SEND)
		{
			ret = EINCOMPLETE;
			errno = EAGAIN;
		}
		else
		{
			client->timeout -= 100 * WAIT_TIMER;
			if (client->timeout <  0)
				ret = EREJECT;
			else
				ret = EINCOMPLETE;
			errno = EAGAIN;
		}
	}
	else if (ret > 0)
	{
		if (FD_ISSET(client->sock, &fds))
		{
			ret = client->ops.status(client);
			if (options & WAIT_SEND)
			{
				ret = ESUCCESS;
			}
			else if (ret != ESUCCESS)
			{
				err("httpclient_wait %p socket closed ", client);
				ret = EREJECT;
			}
		}
	}
	else
	{
		err("httpclient_wait %p error (%d %s)", client, errno, strerror(errno));
		if (errno == EINTR)
		{
			ret = EINCOMPLETE;
			errno = EAGAIN;
		}
		else
			ret = EREJECT;
	}
#else
	if (options & WAIT_SEND)
		ret = ESUCCESS;
	else
		ret = client->ops.status(client);
	/**
	 * The main server loop detected an event on the socket.
	 * If there is not data then the socket had to be closed.
	 */
	if (ret != ESUCCESS)
		ret = EREJECT;
#endif
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
		_buffer_reset(client->sockdata);
	else
		_buffer_shrink(client->sockdata);

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
			recv_ret = client->ops.status(client);
		}
		break;
		case CLIENT_EXIT:
		{
			/**
			 * flush the output socket
			 */
			if (client->ops.flush != NULL)
				client->ops.flush(client);

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
				client->ops.disconnect(client);

			client->state |= CLIENT_STOPPED;
			return ESUCCESS;
		}
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
		if (ret != EINCOMPLETE && (client->request->state & PARSE_MASK) == PARSE_END)
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
				client->request_queue = request->next;
				if ((request->state & PARSE_MASK) < PARSE_END)
				{
					client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
					ret = EINCOMPLETE;
				}
				else if (client->state & CLIENT_LOCKED)
				{
					client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
				}
				else if (!(client->state & CLIENT_KEEPALIVE))
				{
					client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
					ret = EINCOMPLETE;
				}
				else
				{
					client->state = CLIENT_READING | (client->state & ~CLIENT_MACHINEMASK);
				}
				/**
				 * client->request is not null if the reception is not complete.
				 * In this case the client keeps the request until the connection
				 * is closed
				 */
				if (request != client->request)
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
	client->ops.disconnect(client);
	client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
}

/***********************************************************************
 * http_server
 */
static int _httpserver_setmod(http_server_t *server, http_client_t *client)
{
	int ret = ESUCCESS;
	http_server_mod_t *mod = server->mod;
	http_client_modctx_t *currentctx = NULL;
	while (mod)
	{
		http_client_modctx_t *modctx = vcalloc(1, sizeof(*modctx));
		if (modctx == NULL)
		{
			ret = EREJECT;
			break;
		}

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
			if (client->ops.status(client) == ESUCCESS)
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
	count = _httpserver_checkclients(server, prfds, pwfds, pefds);
#ifdef DEBUG
	_debug_maxclients = (_debug_maxclients > count)? _debug_maxclients: count;
	//dbg("nb clients %d / %d / %d", count, _debug_maxclients, _debug_nbclients);
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
					client->ops.destroy(client);
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
		 */

		if ((count + 1) > server->config->maxclients)
			ret = EINCOMPLETE;
	}

	return ret;
}

static int _httpserver_connect(http_server_t *server)
{
	return ESUCCESS;
}

static int _httpserver_run(http_server_t *server)
{
	int ret = ESUCCESS;
	int run = 0;

	server->run = 1;
	run = 1;

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

		if (nbselect == 0)
		{
#ifdef VTHREAD
			//vthread_yield(server->thread);
#else
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
			_httpserver_checkserver(server, prfds, pwfds, pefds);
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
	return ret;
}

static int _maxclients = DEFAULT_MAXCLIENTS;
http_server_t *httpserver_create(http_server_config_t *config)
{
	http_server_t *server;

	server = vcalloc(1, sizeof(*server));
	if (server == NULL)
		return NULL;
	if (config)
		server->config = config;
	else
		server->config = &defaultconfig;
	server->ops = httpserver_ops;
	_httpserver_addmethod(server, str_get, MESSAGE_TYPE_GET);
	_httpserver_addmethod(server, str_post, MESSAGE_TYPE_POST);
	_httpserver_addmethod(server, str_head, MESSAGE_TYPE_HEAD);

	_maxclients += server->config->maxclients;
	nice(-4);
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
	int flags;
	flags = fcntl(server->sock, F_GETFL, 0);
	fcntl(server->sock, F_SETFL, flags | O_NONBLOCK);

	return server;
}

static void _httpserver_addmethod(http_server_t *server, const char *key, _http_message_method_e id)
{
	http_message_method_t *method;
	method = vcalloc(1, sizeof(*method));
	if (method == NULL)
		return;
	method->key = key;
	method->id = id;
	method->next = server->methods;
	server->methods = method;
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
		method = method->next;
	}
	if (method == NULL)
	{
		method = vcalloc(1, sizeof(*method));
		if (method == NULL)
			return;
		method->key = key;
		method->id = id + 1;
		method->next = server->methods;
		server->methods = method;
	}
	if (properties > method->properties)
		method->properties = properties;
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

void httpserver_addconnector(http_server_t *server, char *vhost, http_connector_t func, void *funcarg)
{
	http_connector_list_t *callback;

	callback = vcalloc(1, sizeof(*callback));
	if (callback == NULL)
		return;
	if (vhost)
	{
		int length = strlen(vhost);
		callback->vhost = malloc(length + 1);
		strcpy(callback->vhost, vhost);
	}
	callback->func = func;
	callback->arg = funcarg;
	callback->next = server->callbacks;
	server->callbacks = callback;
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
		if (callback->vhost)
			vfree(callback->vhost);
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
	http_message_method_t *method = server->methods;
	while (method)
	{
		http_message_method_t *next = method->next;
		vfree(method);
		method = next;
	}
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

	if (!strcasecmp(key, "name"))
	{
		value = server->config->hostname;
	}
	else if (!strcasecmp(key, "software"))
	{
		value = httpserver_software;
	}
	else if (!strcasecmp(key, "protocol"))
	{
		value = (char *)httpversion[(server->config->version & HTTPVERSION_MASK)];
	}
	else if (!strcasecmp(key, "port"))
	{
		//snprintf(value, 5, "%d", message->client->server->config->port);
		struct sockaddr_in sin;
		socklen_t len = sizeof(sin);
		if (getsockname(server->sock, (struct sockaddr *)&sin, &len) == 0)
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
		if (getsockname(server->sock, (struct sockaddr *)&sin, &len) == 0)
		{
			getnameinfo((struct sockaddr *) &sin, len,
				host, NI_MAXHOST,
				0, 0, NI_NUMERICHOST);
			value = host;
		}
	}
	return value;
}

const char *httpmessage_SERVER(http_message_t *message, const char *key)
{
	if (message->client == NULL)
		return NULL;
	const char *value = default_value;

	if (!strcasecmp(key, "port"))
	{
		struct sockaddr_in sin;
		socklen_t len = sizeof(sin);
		if (getsockname(message->client->sock, (struct sockaddr *)&sin, &len) == 0)
		{
			getnameinfo((struct sockaddr *) &sin, len,
				0, 0,
				service, NI_MAXSERV, NI_NUMERICSERV);
			value = service;
		}
	}
	else if (!strcasecmp(key, "name"))
	{
		value = httpmessage_REQUEST(message, "Host");
		if (value == NULL)
			value = message->client->server->config->hostname;
	}
	else if (!strcasecmp(key, "addr"))
	{
		struct sockaddr_in sin;
		socklen_t len = sizeof(sin);
		if (getsockname(message->client->sock, (struct sockaddr *)&sin, &len) == 0)
		{
			getnameinfo((struct sockaddr *) &sin, len,
				host, NI_MAXHOST,
				0, 0, NI_NUMERICHOST);
			value = host;
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
			value = message->uri->data;
	}
	else if (!strcasecmp(key, "query"))
	{
		if (message->query_storage != NULL)
			value = message->query_storage->data;
	}
	else if (!strcasecmp(key, "scheme"))
	{
		value = str_defaultscheme;
	}
	else if (!strcasecmp(key, "version"))
	{
		value = httpversion[(message->version & HTTPVERSION_MASK)];
	}
	else if (!strcasecmp(key, "method"))
	{
		if (message->method)
			value = (char *)message->method->key;
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
		if (message->content != NULL)
		{
			value = message->content_type;
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
		dbentry_t *header = message->headers;
		while (header != NULL)
		{
			if (!strcasecmp(header->key, key))
			{
				value = header->value;
				break;
			}
			header = header->next;
		}
		if (header == NULL)
		{
			dbg("header %s not found", key);
		}
	}
	return value;
}

http_server_session_t *_httpserver_createsession(http_server_t *server, http_client_t *client)
{
	http_server_session_t *session = NULL;
	session = vcalloc(1, sizeof(*session));
	if (session)
		session->storage = _buffer_create(MAXCHUNKS_SESSION, server->config->chunksize);
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
				_buffer_append(message->client->session->storage, key, strlen(key) + 1);
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
