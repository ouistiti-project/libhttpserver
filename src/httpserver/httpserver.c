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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "valloc.h"
#include "vthread.h"
#include "dbentry.h"
#include "httpserver.h"
#include "_httpserver.h"
#define _HTTPMESSAGE_
#include "_httpmessage.h"

extern httpserver_ops_t *httpserver_ops;

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
# define dbg(...)
#endif

struct http_connector_list_s
{
	char *vhost;
	http_connector_t func;
	void *arg;
	struct http_connector_list_s *next;
};

struct http_server_mod_s
{
	void *arg;
	http_getctx_t func;
	http_freectx_t freectx;
	http_server_mod_t *next;
};

struct http_client_modctx_s
{
	void *ctx;
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

const char *str_get = "GET";
const char *str_post = "POST";
const char *str_head = "HEAD";

static char _httpserver_software[] = "libhttpserver";
char *httpserver_software = _httpserver_software;
/********************************************************************/
#define BUFFERMAX 2048
static int ChunkSize = 0;
static buffer_t * _buffer_create(int nbchunks, int chunksize)
{
	buffer_t *buffer = vcalloc(1, sizeof(*buffer));
	/**
	 * nbchunks is unused here, because is it possible to realloc.
	 * Embeded version may use the nbchunk with special vcalloc.
	 * The idea is to create a pool of chunks into the stack.
	 */
	buffer->data = vcalloc(1, chunksize);
	/**
	 * the chunksize has to be constant during the life of the application.
	 * Two ways are available:
	 *  - to store the chunksize into each buffer (takes a lot of place).
	 *  - to store into a global variable (looks bad).
	 */
	if (ChunkSize == 0)
		ChunkSize = chunksize;
	buffer->maxchunks = nbchunks;
	buffer->size = chunksize;
	buffer->offset = buffer->data;
	return buffer;
}

static char *_buffer_append(buffer_t *buffer, const char *data, int length)
{
	if (buffer->data + buffer->size <= buffer->offset + length + 1)
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
	message->method = calloc(1, sizeof(*message->method));
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
	if (message->headers_storage)
		_buffer_destroy(message->headers_storage);
	dbentry_t *header = message->headers;
	while (header)
	{
		dbentry_t *next = header->next;
		free(header);
		header = next;
	}
	vfree(message);
}

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
					warn("parse reject method %s", data->offset);
					data->offset++;
					message->version = message->client->server->config->version;
					message->result = RESULT_405;
					ret = EREJECT;
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
							message->query = message->uri->data + length + 1;
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
						ret = EREJECT;
						warn("parse reject uri too long 2: %s %s", message->uri->data, data->data);
					}
				}
				if (next != PARSE_URI)
				{
					if (message->uri->length > 0)
					{
						if (message->query == NULL)
							message->query = message->uri->data + message->uri->length;
						dbg("new request %s %s", message->method->key, message->uri->data);
					}
					else
					{
						message->version = message->client->server->config->version;
						message->result = RESULT_400;
						ret = EREJECT;
						warn("parse reject uri too short");
					}
				}
			}
			break;
			case PARSE_STATUS:
			{
				int i;
				for (i = HTTP09; i < HTTPVERSIONS; i++)
				{
					int length = strlen(_http_message_version[i]);
					if (!strncasecmp(data->offset, _http_message_version[i], length))
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
					int length = strlen(_http_message_version[i]);
					if (!strncasecmp(version, _http_message_version[i], length))
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
							ret = EREJECT;
							message->result = RESULT_400;
							warn("bad request %s", data->data);
						}
						message->version = i;
						break;
					}
				}
				if (i == HTTPVERSIONS)
				{
					ret = EREJECT;
					message->result = RESULT_400;
					warn("request bad protocol version %s", version);
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
					ret = EREJECT;
					message->result = RESULT_400;
					warn("request bad header %s", message->headers_storage);
				}
				else if (message->content_length == 0)
				{
					next = PARSE_END;
					dbg("no content inside request");
				}
				else
				{
					next = PARSE_PRECONTENT;
				}
			}
			break;
			case PARSE_PRECONTENT:
			{
				if (!(message->state & PARSE_CONTINUE))
				{
					message->state |= PARSE_CONTINUE;
				}
				else
				{
					next = PARSE_CONTENT;
					message->state &= ~PARSE_CONTINUE;
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
					//int length = data->length -(data->offset - data->data);
					int length = data->length;
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
			case PARSE_END:
			{
				ret = ESUCCESS;
			}
			break;
		}
		if (next == (message->state & PARSE_MASK) && (ret == ECONTINUE))
		{
			if (next < PARSE_CONTENT)
				ret = EINCOMPLETE;
			break;
		}
		message->state = (message->state & ~PARSE_MASK) | next;
		if (ret == EREJECT)
			message->state = PARSE_END;
	} while (ret == ECONTINUE);
	return ret;
}

HTTPMESSAGE_DECL int _httpmessage_buildresponse(http_message_t *message, int version, buffer_t *header)
{
	http_message_version_e _version = message->version;
	if (message->version > (version & HTTPVERSION_MASK))
		_version = (version & HTTPVERSION_MASK);
	_buffer_append(header, _http_message_version[_version], strlen(_http_message_version[_version]));
	char *status = _httpmessage_status(message);
	dbg("response %s", status);
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
		if ((message->content_length > 0) &&
			(!strcmp(headers->key, str_contentlength)))
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
	if (message->content_length > 0)
	{
		if (message->mode & HTTPMESSAGE_KEEPALIVE > 0)
		{
			char keepalive[32];
			snprintf(keepalive, 31, "%s: %s\r\n", str_connection, "Keep-Alive");
			_buffer_append(header, keepalive, strlen(keepalive));
		}
		char content_length[32];
		snprintf(content_length, 31, "%s: %d\r\n", str_contentlength, message->content_length);
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

int httpmessage_content(http_message_t *message, char **data, int *size)
{
	*size = 0;
	if (message->content)
	{
		*data = message->content->data;
		*size = message->content->length;
	}
	return message->content_length + *size;
}

int httpmessage_parsecgi(http_message_t *message, char *data, int *size)
{
	static buffer_t tempo;
	tempo.data = data;
	tempo.offset = data;
	tempo.length = *size;
	tempo.size = *size;
	if (message->state == PARSE_INIT)
		message->state = PARSE_STATUS;
	int ret = _httpmessage_parserequest(message, &tempo);
	*size = tempo.length - (tempo.offset - tempo.data);
	if (*size > 0)
		_buffer_shrink(&tempo);
	if ((message->state & PARSE_MASK) > PARSE_PRECONTENT)
		ret = ECONTINUE;
	if (message->state == PARSE_END)
	{
		if (*size > 0)
		{
			warn("end with size %d not 0", *size);
			*size = 0;
		}
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
				return EREJECT;
			if (key[0] != 0)
				_httpmessage_addheader(message, key, value);
			key = storage->data + i + 1;
			value = NULL;
		}
	}
	return ESUCCESS;
}

void httpmessage_addheader(http_message_t *message, char *key, char *value)
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
		if (!strncasecmp(key, "Status", 6))
		{
			int result;
			sscanf(value,"%d",&result);
			httpmessage_result(message, result);
		}
	}
}

char *httpmessage_addcontent(http_message_t *message, char *type, char *content, int length)
{
	if (message->content == NULL)
	{
		if (type == NULL)
		{
			httpmessage_addheader(message, (char *)str_contenttype, "text/plain");
		}
		else if (strcmp(type, "none"))
		{
			httpmessage_addheader(message, (char *)str_contenttype, type);
		}
	}
	if (message->content == NULL && content != NULL)
		message->content = _buffer_create(MAXCHUNKS_CONTENT, message->client->server->config->chunksize);

	if (content != NULL)
	{
		if (length == -1)
			length = strlen(content);
		_buffer_append(message->content, content, length);
	}
	if (message->content_length == 0)
	{
		message->content_length = length;
	}
	if (message->content != NULL && message->content->data != NULL )
		return message->content->data;
	return NULL;
}

char *httpmessage_appendcontent(http_message_t *message, char *content, int length)
{
	if (message->content != NULL && content != NULL)
	{
		if (length == -1)
			length = strlen(content);
		if (length + message->content->length <= message->content->size)
		{
			_buffer_append(message->content, content, length);
			message->content_length += length;
			return message->content->data;
		}
	}
	return NULL;
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

void httpclient_addconnector(http_client_t *client, char *vhost, http_connector_t func, void *funcarg)
{
	http_connector_list_t *callback;

	callback = vcalloc(1, sizeof(*callback));
	if (vhost)
	{
		int length = strlen(vhost);
		callback->vhost = malloc(length + 1);
		strcpy(callback->vhost, vhost);
	}

	callback->func = func;
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

http_client_t *httpclient_create(http_server_t *server, httpclient_ops_t *fops, int chunksize)
{
	http_client_t *client = vcalloc(1, sizeof(*client));
	client->server = server;

	if (server)
	{
		http_connector_list_t *callback = server->callbacks;
		while (callback != NULL)
		{
			httpclient_addconnector(client, callback->vhost, callback->func, callback->arg);
			callback = callback->next;
		}
	}
	memcpy(&client->ops, fops, sizeof(client->ops));
	client->ctx = client;
	client->sockdata = _buffer_create(1, chunksize);

	return client;
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
	char *method = httpmessage_REQUEST(request, "method");
	_buffer_append(data, method, strlen(method));
	_buffer_append(data, " ", 1);
	char *uri = httpmessage_REQUEST(request, "uri");
	_buffer_append(data, uri, strlen(uri));
	char *version = httpmessage_REQUEST(request, "version");
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

static void _httpclient_destroy(http_client_t *client)
{
	http_connector_list_t *callback = client->callbacks;
	while (callback != NULL)
	{
		http_connector_list_t *next = callback->next;
		if (callback->vhost)
			free(callback->vhost);
		free(callback);
		callback = next;
	}
	dbentry_t *session = client->session;
	while (session)
	{
		dbentry_t *next = session->next;
		free(session);
		session = next;
	}
	if (client->sockdata)
		_buffer_destroy(client->sockdata);
	if (client->request)
		_httpmessage_destroy(client->request);
	if (client->session_storage)
		vfree(client->session_storage);
	vfree(client);
}

static int _httpclient_connect(http_client_t *client)
{

	client->state &= ~CLIENT_STARTED;
	client->state |= CLIENT_RUNNING;
	do
	{
		_httpclient_run(client);
	} while(!(client->state & CLIENT_STOPPED));
	client->state = CLIENT_DEAD;
	dbg("client %p close", client);
#ifdef DEBUG
	fflush(stderr);
#endif
	return 0;
}

int httpclient_socket(http_client_t *client)
{
	return client->sock;
}

static int _httpclient_checkconnector(http_client_t *client, http_message_t *request, http_message_t *response)
{
	int ret = ESUCCESS;
	char *vhost = NULL;
	http_connector_list_t *iterator = request->connector;

	if (iterator != NULL)
	{
		ret = iterator->func(iterator->arg, request, response);
		if (ret == ESUCCESS)
		{
			client->state |= CLIENT_RESPONSEREADY;
		}
		return ret;
	}

	iterator = client->callbacks;
	while (iterator != NULL)
	{
		vhost = iterator->vhost;
		if (vhost != NULL)
		{
			char *host = httpmessage_REQUEST(request, "host");
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

static int _httpclient_runconnector(http_client_t *client, http_message_t *request, http_message_t *response)
{
	http_connector_list_t *connector = request->connector;
	if (connector && connector->func)
		return connector->func(connector->arg, request, response);
	return EREJECT;
}

static int _httpclient_request(http_client_t *client)
{
	/**
	 * By default the function has to wait more data on the socket
	 * before to be call again.
	 *  ECONTINUE means treatment (here is more data) is needed.
	 *  EINCOMPLETE means the function needs to be call again ASAP.
	 * This is the same meaning with the connectors.
	 */
	int ret = ECONTINUE;
	int size = 0;
	if (client->request == NULL)
		client->request = _httpmessage_create(client, NULL, client->server->config->chunksize);

	/**
	 * here, it is the call to the recvreq callback from the
	 * server configuration.
	 * see http_server_config_t and httpserver_create
	 */
	if (client->sockdata->length <= (client->sockdata->offset - client->sockdata->data))
		_buffer_reset(client->sockdata);
	else
		_buffer_shrink(client->sockdata);
	size = client->ops.recvreq(client->ctx, client->sockdata->offset, client->sockdata->size - client->sockdata->length - 1);
	if (size > 0)
	{
		client->sockdata->length += size;
		client->sockdata->data[client->sockdata->length] = 0;
	}
	else if (size == EINCOMPLETE)
		return ECONTINUE;
	else
		return EREJECT;

	/**
	 * the receiving may complete the buffer, but the parser has
	 * to check the whole buffer.
	 **/
	client->sockdata->offset = client->sockdata->data;
	ret = _httpmessage_parserequest(client->request, client->sockdata);
	if (ret == EREJECT)
	{
		if (client->request->response == NULL)
		{
			/**
			 * parsing error before response creation 
			 * response's result will be the result set into the request.
			 **/
			client->request->response = _httpmessage_create(client, client->request, client->server->config->chunksize);
		}
		/**
		 * the parsing found an error in the request
		 * the treatment is completed and is successed
		 **/
		return ESUCCESS;
	}

	/**
	 * The request is partially read.
	 * The connector can start to read the request when the header is ready.
	 * A problem is the size of the header. It is impossible to start
	 * the treatment before the end of the header, and it needs to
	 * store the header informations. It takes some place in  memory,
	 * depending of the server. It may be dangerous, a hacker can send
	 * a request with a very big header.
	 */
	if ((client->request->state & PARSE_MASK) > PARSE_POSTHEADER)
	{
		if (client->request->response == NULL)
			client->request->response = _httpmessage_create(client, client->request, client->server->config->chunksize);
		ret = _httpclient_checkconnector(client, client->request, client->request->response);
		if (ret == EREJECT)
		{
#ifdef DEBUG
			if (client->request->response->result != RESULT_200)
				err("Result error may return ESUCCESS");
#endif
			client->request->response->result = RESULT_404;
			warn("request not found %s", client->request->uri->data);
		}
	}
	/**
	 * The request's content should be used by  "_httpclient_checkconnector"
	 * if it is required. After that the content is not stored and useless.
	 * The content is the "tempo" buffer, it is useless to free it.
	 **/
	client->request->content = NULL;

	switch (ret)
	{
		case ESUCCESS:
		{
			if (!(client->state & CLIENT_RESPONSEREADY))
			{
				client->request->result = RESULT_405;
			}
		}
		break;
		case EREJECT:
		{
			ret = ESUCCESS;
		}
		break;
		case ECONTINUE:
		case EINCOMPLETE:
		{
			if ((client->request->state & PARSE_MASK) == PARSE_END)
				ret = ESUCCESS;
		}
		break;
	}
	return ret;
}

static int _httpclient_pushrequest(http_client_t *client, http_message_t *request)
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
	return (request->result != RESULT_200)? EREJECT:ESUCCESS;
}

int httpclient_wait(http_client_t *client, int sending)
{
	int ret = client->sock;
#if defined(VTHREAD) && !defined(BLOCK_SOCKET)
	struct timeval *ptimeout = NULL;
	struct timeval timeout;
	fd_set fds;
	fd_set *rfds = NULL, *wfds = NULL;
	if (client->server->config->keepalive)
	{
		if (sending)
		{
			timeout.tv_sec = 0;
			timeout.tv_usec = 100;
		}
		else
		{
			timeout.tv_sec = client->server->config->keepalive;
			timeout.tv_usec = 0;
		}
		ptimeout = &timeout;
	}
	FD_ZERO(&fds);
	FD_SET(client->sock, &fds);
	if (sending)
		wfds = &fds;
	else
		rfds = &fds;
	ret = select(client->sock + 1, rfds, wfds, NULL, ptimeout);
	if (ret == 0)
	{
		ret = -EINCOMPLETE;
		errno = EAGAIN;
	}
	else if (ret > 0)
	{
		if (FD_ISSET(client->sock, &fds))
			ret = client->sock;
		else
			ret = EREJECT;
	}
	else
		ret = EREJECT;
#endif
	return ret;
}

static int _httpclient_run(http_client_t *client)
{
	http_message_t *request = NULL;
	if (client->request_queue)
		request = client->request_queue;

	int request_ret = ECONTINUE;
	if ((!(client->state & CLIENT_LOCKED) &&
		(client->server->config->version >= (HTTP11 | HTTP_PIPELINE))) || 
		((client->state & CLIENT_MACHINEMASK) < CLIENT_PUSHREQUEST))
	{
		request_ret = _httpclient_request(client);
		if (request_ret == ESUCCESS)
		{
			_httpclient_pushrequest(client, client->request);
		}
	}

	switch (client->state & CLIENT_MACHINEMASK)
	{
		case CLIENT_NEW:
		{
			client->state &= ~CLIENT_RESPONSEREADY;
			if (request_ret == ESUCCESS)
			{
				client->state = CLIENT_PUSHREQUEST | (client->state & ~CLIENT_MACHINEMASK);
			}
			else
				client->state = CLIENT_REQUEST | (client->state & ~CLIENT_MACHINEMASK);
			if (request_ret == ECONTINUE)
			{
				if (httpclient_wait(client, 0) < 0)
				{
					/* timeout */
					client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
					client->state &= ~CLIENT_KEEPALIVE;
				}
			}
		}
		break;
		case CLIENT_REQUEST:
		{
			if (request_ret == ESUCCESS)
			{
				client->state = CLIENT_PUSHREQUEST | (client->state & ~CLIENT_MACHINEMASK);
			}
			else if (request_ret == EREJECT)
			{
				client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
			}
			else if (request_ret == ECONTINUE)
			{
				if (httpclient_wait(client, 0) < 0)
				{
					/* timeout */
					client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
					client->state &= ~CLIENT_KEEPALIVE;
				}
			}
		}
		break;
		case CLIENT_PUSHREQUEST:
		{
			if (client->request->response->result > 299)
				client->state = CLIENT_PARSERERROR | (client->state & ~CLIENT_MACHINEMASK);
			else if (client->request->response->content == NULL && client->request->response->content_length == 0)
				client->state = CLIENT_PARSER1 | (client->state & ~CLIENT_MACHINEMASK);
			else if (client->request->version == HTTP09)
				client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
			else
				client->state = CLIENT_RESPONSEHEADER | (client->state & ~CLIENT_MACHINEMASK);
			if (client->request->mode & HTTPMESSAGE_KEEPALIVE)
			{
				client->state |= CLIENT_KEEPALIVE;
			}
			if (client->request->response->mode & HTTPMESSAGE_KEEPALIVE)
			{
				client->state |= CLIENT_KEEPALIVE;
			}
			if (client->request->mode & HTTPMESSAGE_LOCKED)
			{
				client->state |= CLIENT_LOCKED;
			}
			if (client->request->response->mode & HTTPMESSAGE_LOCKED)
			{
				client->state |= CLIENT_LOCKED;
			}
			/**
			 * The request was pushed to the request_queue with 
			 * _httpclient_pushrequest. The next loop will unqueue
			 * this request. Here the client->request is free to be
			 * reused for a new request.
			 */
			client->request = NULL;
		}
		break;
		case CLIENT_PARSER1:
		{
			int ret = _httpclient_runconnector(client, request, request->response);
			if (ret == EREJECT)
			{
				client->state = CLIENT_PARSERERROR | (client->state & ~CLIENT_MACHINEMASK);
				/** delete func to stop request after the error response **/
				request->connector = NULL;
			}
			else if (ret != EINCOMPLETE)
			{
				if (ret == ESUCCESS)
					client->state |= CLIENT_RESPONSEREADY;
				if (request->response->version == HTTP09)
					client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
				else
					client->state = CLIENT_RESPONSEHEADER | (client->state & ~CLIENT_MACHINEMASK);
			}
		}
		break;
		case CLIENT_PARSER2:
		{
			int ret = _httpclient_runconnector(client, request, request->response);
			if (ret == EREJECT)
			{
				/**
				 * the connector rejects now the request, then an error occured
				 * and the connector is not able to continue
				 */
				/** delete func to stop request after the error response **/
				request->connector = NULL;
				client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
			}
			else if (ret != EINCOMPLETE &&
					request->response->content &&
					request->response->content->length > 0)
			{
				if (ret == ESUCCESS)
					client->state |= CLIENT_RESPONSEREADY;
				/**
				 * the connector return ESUCCESS or ECONTINUE, then it
				 * allows the connection to change state
				 * on EINCOMPLETE the state has to stay in this value
				 */
				/**
				 * on ESUCCESS or ECONTINUE if some value is ready
				 * it is required to send them
				 */
				/**
				 * an empty file may have not content
				 */
				client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
			}
			else if (ret == ESUCCESS)
				client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
		}
		break;
		case CLIENT_RESPONSEHEADER:
		{
			int size = 0;
			buffer_t *header = _buffer_create(MAXCHUNKS_HEADER, client->server->config->chunksize);
			_httpmessage_buildresponse(request->response, client->server->config->version, header);
			while (header->length > 0)
			{
				/**
				 * here, it is the call to the sendresp callback from the
				 * server configuration.
				 * see http_server_config_t and httpserver_create
				 */
				size = client->ops.sendresp(client->ctx, header->offset, header->length);
				if (size == EINCOMPLETE)
					continue;
				if (size == EREJECT)
					break;
				header->offset += size;
				header->length -= size;
			}
			_buffer_reset(header);
			_httpmessage_buildheader(request->response, header);
			while (header->length > 0)
			{
				/**
				 * here, it is the call to the sendresp callback from the
				 * server configuration.
				 * see http_server_config_t and httpserver_create
				 */
				size = client->ops.sendresp(client->ctx, header->offset, header->length);
				if (size == EINCOMPLETE)
					continue;
				if (size == EREJECT)
					break;
				header->offset += size;
				header->length -= size;
			}
			do
			{
				size = client->ops.sendresp(client->ctx, "\r\n", 2);
			}
			while (size == EINCOMPLETE);
			if (size == EREJECT)
			{
				client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
			}
			else if (client->state & CLIENT_RESPONSEREADY)
				client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
			else if (request->response->content &&
					request->response->content->length > 0)
				client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
			else
				client->state = CLIENT_PARSER2 | (client->state & ~CLIENT_MACHINEMASK);
			_buffer_destroy(header);
		}
		break;
		case CLIENT_RESPONSECONTENT:
		{
			int size = 0;
			if (request->response->content)
			{
				request->response->content->offset = request->response->content->data;
				while (request->method->id != MESSAGE_TYPE_HEAD &&
						request->response->content->length > 0)
				{
					size = client->ops.sendresp(client->ctx, request->response->content->offset, request->response->content->length);
					if (size  > 0 && size != request->response->content->length)
					{
						request->response->content->length -= size;
						request->response->content->offset += size;
					}
					else if (size == EINCOMPLETE)
						continue;
					else
						break;
				}
				_buffer_reset(request->response->content);
			}
			if (client->state & CLIENT_RESPONSEREADY)
				client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
			else if (client->state & CLIENT_LOCKED)
			{
				int ret = _httpclient_runconnector(client, request, request->response);
				if (ret == ECONTINUE)
					size = 1;
				else if (ret == EREJECT)
					client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
			}
			else if (size <= 0 && errno != EAGAIN)
			{
				client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
			}
			else
				client->state = CLIENT_PARSER2 | (client->state & ~CLIENT_MACHINEMASK);
		}
		break;
		case CLIENT_PARSERERROR:
		{
			if (request->response->result == RESULT_200)
				request->response->result = RESULT_400;
			if (request->response->content == NULL)
			{
				const char *value = _httpmessage_status(request->response);
				httpmessage_addcontent(request->response, "text/plain", (char *)value, strlen(value));
				client->state |= CLIENT_RESPONSEREADY;
			}
			if (request->response->version == HTTP09)
				client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
			else
				client->state = CLIENT_RESPONSEHEADER | (client->state & ~CLIENT_MACHINEMASK);
			client->state &= ~CLIENT_KEEPALIVE;
		}
		break;
		case CLIENT_COMPLETE:
		{
			/**
			 * flush the input socket to begin the next request
			 */
			while (request && request->state < PARSE_END)
			{
				int ret;
				int size = 0;
				/**
				 * here, it is the call to the recvreq callback from the
				 * server configuration.
				 * see http_server_config_t and httpserver_create
				 */
				if (client->sockdata->size <= client->sockdata->length)
					_buffer_reset(client->sockdata);
				size = client->ops.recvreq(client->ctx, client->sockdata->offset, client->sockdata->size - client->sockdata->length - 1);
				if (size > 0)
				{
					client->sockdata->length += size;
					client->sockdata->data[client->sockdata->length] = 0;
				}
				else if (size != EINCOMPLETE)
				{
					client->state |= ~CLIENT_KEEPALIVE;
				}

				/**
				 * the receiving may complete the buffer, but the parser has
				 * to check the whole buffer.
				 **/
				client->sockdata->offset = client->sockdata->data;
				_httpmessage_parserequest(request, client->sockdata);
			}
			/**
			 * flush the output socket
			 */
			if (client->ops.flush != NULL)
				client->ops.flush(client);
			/**
			 * to stay in keep alive the rules are:
			 *  - the server has to be configurated;
			 *  - the request uses the protocol HTTP11 and over
			 *  - the request asks to stay in keep alive mode
			 *  - the response is understandable by the client
			 *     (the webbrowser nees to know when the response is complete,
			 *      then the response needs a content length).
			 */
			if (client->state & CLIENT_LOCKED)
			{
				client->state |= CLIENT_STOPPED;
			}
			else if (client->server->config->keepalive &&
				(client->state & CLIENT_KEEPALIVE) &&
				request && request->response->version > HTTP10)
			{
				client->state = CLIENT_NEW | (client->state & ~CLIENT_MACHINEMASK);
				dbg("keepalive %p", client);
			}
			else
			{
				client->state = CLIENT_EXIT | (client->state & ~CLIENT_MACHINEMASK);
			}
			if (client->request_queue)
			{
				http_message_t *next = client->request_queue->next;
				_httpmessage_destroy(client->request_queue);
				client->request_queue = next;
			}
		}
		break;
		case CLIENT_EXIT:
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
			client->ops.close(client);
			client->state |= CLIENT_STOPPED;
		}
		break;
	}
	return 0;
}

void httpclient_shutdown(http_client_t *client)
{
	client->ops.close(client);
}

/***********************************************************************
 * http_server
 */
static int _httpserver_connect(http_server_t *server)
{
	int ret = 0;
	int maxfd = 0;
	fd_set rfds, wfds;

	server->run = 1;
	while(server->run)
	{
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_SET(server->sock, &rfds);
		maxfd = server->sock;
		http_client_t *client = server->clients;

		while (client != NULL)
		{
			if ((client->state & CLIENT_MACHINEMASK) == CLIENT_DEAD)
			{
				dbg("server try join %p", client);
#ifdef VTHREAD
				vthread_join(client->thread, NULL);
#endif
				dbg("client %p died", client);

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
			else if (httpclient_socket(client) > 0)
			{
#ifndef VTHREAD
				if ((client->state & CLIENT_MACHINEMASK) != CLIENT_REQUEST)
				{
					FD_SET(httpclient_socket(client), &wfds);
				}
				else
					FD_SET(httpclient_socket(client), &rfds);
#else
				FD_SET(httpclient_socket(client), &rfds);
#endif
				maxfd = (maxfd > httpclient_socket(client))? maxfd:httpclient_socket(client);
				client = client->next;
			}
			else
				client = client->next;
		}
		struct timeval *ptimeout = NULL;
		struct timeval timeout;
		if (server->config->keepalive)
		{
			timeout.tv_sec = server->config->keepalive;
			timeout.tv_usec = 0;
			ptimeout = &timeout;
		}
		ret = select(maxfd +1, &rfds, &wfds, NULL, ptimeout);
		if (ret > 0)
		{
			if (FD_ISSET(server->sock, &rfds))
			{
				http_client_t *client = server->ops->createclient(server);
				dbg("new connection %p", client);
				http_server_mod_t *mod = server->mod;
				http_client_modctx_t *currentctx = NULL;
				ret = 0;
				while (mod)
				{
					http_client_modctx_t *modctx = vcalloc(1, sizeof(*modctx));

					if (mod->func)
					{
						modctx->ctx = mod->func(mod->arg, client, (struct sockaddr *)&client->addr, client->addr_size);
						if (modctx->ctx == NULL)
							ret = 1;
					}
					modctx->freectx = mod->freectx;
					mod = mod->next;
					if (client->modctx == NULL)
						client->modctx = modctx;
					else
					{
						currentctx->next = modctx;
					}
					currentctx = modctx;
				}
				if (ret == 0)
				{
					int flags;
					flags = fcntl(httpclient_socket(client), F_GETFL, 0);
					fcntl(httpclient_socket(client), F_SETFL, flags | O_NONBLOCK);

					client->next = server->clients;
					server->clients = client;

					ret = 1;
				}
			}
			else
			{
				client = server->clients;
				while (client != NULL)
				{
					http_client_t *next = client->next;
					if (httpclient_socket(client) < 0)
					{
						client->state |= CLIENT_STOPPED;
					}
					else if (FD_ISSET(httpclient_socket(client), &rfds) || FD_ISSET(httpclient_socket(client), &wfds))
					{
#ifndef VTHREAD
						client->state |= CLIENT_RUNNING;
						_httpclient_run(client);
#else
						if (!(client->state & (CLIENT_STARTED | CLIENT_RUNNING)))
						{
							vthread_attr_t attr;
							client->state &= ~CLIENT_STOPPED;
							client->state |= CLIENT_STARTED;
							vthread_create(&client->thread, &attr, (vthread_routine)_httpclient_connect, (void *)client, sizeof(*client));
						}
						else
						{
							if (!vthread_exist(client->thread))
							{
								client->state |= CLIENT_STOPPED;
							}
							vthread_yield(client->thread);
						}
#endif
					}
					client = next;
				}
			}
		}
#ifndef VTHREAD
		/**
		 * TODO: this code has to be checked and explained
		 */
		else if (ret == 0)
		{
			client = server->clients;
			while (client != NULL)
			{
				http_client_t *next = client->next;
				client->state &= ~CLIENT_KEEPALIVE;
				_httpclient_run(client);
				client = next;
			}
		}
#endif
	}
	server->ops->close(server);
	return ret;
}

http_server_t *httpserver_create(http_server_config_t *config)
{
	http_server_t *server;

	server = vcalloc(1, sizeof(*server));
	if (config)
		server->config = config;
	else
		server->config = &defaultconfig;
	server->ops = httpserver_ops;
	_httpserver_addmethod(server, str_get, MESSAGE_TYPE_GET);
	_httpserver_addmethod(server, str_post, MESSAGE_TYPE_POST);
	_httpserver_addmethod(server, str_head, MESSAGE_TYPE_HEAD);

#ifdef WIN32
	WSADATA wsaData = {0};
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
	if (server->ops->start(server))
	{
		free(server);
		return NULL;
	}
	return server;
}

static void _httpserver_addmethod(http_server_t *server, const char *key, _http_message_method_e id)
{
	http_message_method_t *method;
	method = vcalloc(1, sizeof(*method));
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
		method->key = key;
		method->id = id + 1;
		method->next = server->methods;
		server->methods = method;
	}
	if (properties > method->properties)
		method->properties = properties;
}

void httpserver_addmod(http_server_t *server, http_getctx_t modf, http_freectx_t unmodf, void *arg)
{
	http_server_mod_t *mod = vcalloc(1, sizeof(*mod));
	mod->func = modf;
	mod->freectx = unmodf;
	mod->arg = arg;
	mod->next = server->mod;
	server->mod = mod;
}

void httpserver_addconnector(http_server_t *server, char *vhost, http_connector_t func, void *funcarg)
{
	http_connector_list_t *callback;
	
	callback = vcalloc(1, sizeof(*callback));
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
	vthread_attr_t attr;

#ifndef VTHREAD
	_httpserver_connect(server);
#else
	vthread_create(&server->thread, &attr, (vthread_routine)_httpserver_connect, (void *)server, sizeof(*server));
#endif
}

void httpserver_disconnect(http_server_t *server)
{
	if (server->thread)
	{
		server->run = 0;
		server->ops->close(server);
	}
}

void httpserver_destroy(http_server_t *server)
{
	httpserver_disconnect(server);
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
		http_server_mod_t  *next = mod->next;
		vfree(mod);
		mod = next;
	}
	vfree(server);
#ifdef WIN32
	WSACleanup();
#endif
}
/***********************************************************************/

static char default_value[8] = {0};
static char host[NI_MAXHOST], service[NI_MAXSERV];
char *httpserver_INFO(http_server_t *server, char *key)
{
	char *value = default_value;
	memset(default_value, 0, sizeof(default_value));

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
		value = (char *)_http_message_version[(server->config->version & HTTPVERSION_MASK)];
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

char *httpmessage_COOKIE(http_message_t *message, char *key)
{
	char *value = NULL;
	if (message->client == NULL)
		return NULL;
	if (message->cookies == NULL)
	{
		char *cookie = NULL;
		/**
		 * same as httpmessage_REQUEST(message, "Cookie")
		 * but disallow header access with
		 * header->key = "#ookie";
		 **/
		dbentry_t *header = message->headers;
		while (header != NULL)
		{
			if (!strcasecmp(header->key, "Cookie"))
			{
				header->key = "#ookie";
				cookie = header->value;
				break;
			}
			header = header->next;
		}
		if (cookie && cookie[0] != '\0')
		{
			while (*cookie == ' ')
			{
				cookie++;
			}
			char *key = cookie;
			int length = strlen(cookie) + 1;
			char *value = NULL;
			int i;
			for (i = 0; i < length; i++)
			{
				if (cookie[i] == '=' && value == NULL)
				{
					cookie[i] = '\0';
					value = cookie + i + 1;
					while (*value == ' ')
					{
						i++;
						value++;
					}
				}
				else if (cookie[i] == '\0' || cookie[i] == ';')
				{
					if (key[0] != 0 && value != NULL)
					{
						dbg("new cookie: %s => %s", key, value);
						dbentry_t *headerinfo;
						headerinfo = vcalloc(1, sizeof(dbentry_t));
						headerinfo->key = key;
						headerinfo->value = value;
						headerinfo->next = message->cookies;
						message->cookies = headerinfo;
					}
					key = cookie + i + 1;
					while (*key == ' ')
					{
						i++;
						key++;
					}
					value = NULL;
				}
			}
		}
	}
	if (message->cookies != NULL)
	{
		dbentry_t *cookie = message->cookies;
		while (cookie != NULL)
		{
			if (!strcasecmp(cookie->key, key))
			{
				value = cookie->value;
				break;
			}
			cookie = cookie->next;
		}
	}
	return value;
}

char *httpmessage_SERVER(http_message_t *message, char *key)
{
	if (message->client == NULL)
		return NULL;
	char *value = default_value;
	memset(default_value, 0, sizeof(default_value));

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

char *httpmessage_REQUEST(http_message_t *message, char *key)
{
	char *value = default_value;
	if (!strcasecmp(key, "uri"))
	{
		if (message->uri != NULL)
			value = message->uri->data;
	}
	else if (!strcasecmp(key, "query"))
	{
		if (message->query != NULL)
			value = message->query;
	}
	else if (!strcasecmp(key, "scheme"))
	{
		strcpy(value, "http");
	}
	else if (!strcasecmp(key, "version"))
	{
		strcpy(value, _http_message_version[(message->version & HTTPVERSION_MASK)]);
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
	else if (!strncasecmp(key, "remote_addr", 11))
	{
		if (message->client == NULL)
			return NULL;

		getnameinfo((struct sockaddr *) &message->client->addr, sizeof(message->client->addr),
			host, NI_MAXHOST, 0, 0, NI_NUMERICHOST);
		value = host;
	}
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
	}
	return value;
}

char *httpmessage_SESSION(http_message_t *message, char *key, char *value)
{
	dbentry_t *sessioninfo;
	if (message->client == NULL)
		return NULL;

	sessioninfo = message->client->session;
	
	while (sessioninfo && strcmp(sessioninfo->key, key))
	{
		sessioninfo = sessioninfo->next;
	}
	if (value != NULL)
	{
		if (!sessioninfo)
		{
			sessioninfo = vcalloc(1, sizeof(*sessioninfo));
			if (!message->client->session_storage)
			{
				message->client->session_storage = _buffer_create(MAXCHUNKS_SESSION, message->chunksize);
			}
			sessioninfo->key = 
				_buffer_append(message->client->session_storage, key, strlen(key) + 1);
			sessioninfo->next = message->client->session;
			message->client->session = sessioninfo;
		}
		if (sessioninfo->value)
		{
			dbentry_t *next = sessioninfo->next;
			if (strlen(value) <= strlen(sessioninfo->value))
			{
				strcpy(sessioninfo->value, value);
			}
			else if (next != NULL)
			{
				char *data = message->client->session_storage->data;
				int length = message->client->session_storage->length;
				length -= next->key - data;
				memmove(sessioninfo->key, next->key, length);
				length = next->key - sessioninfo->key;
				while (next != NULL)
				{
					next->key -= length;
					next->value -= length;
					next = next->next;
				}
				message->client->session_storage->length -= length;
			}
		}
		else
			sessioninfo->value = 
				_buffer_append(message->client->session_storage, value, strlen(value) + 1);
	}
	else if (sessioninfo == NULL)
		return default_value;
	return sessioninfo->value;
}
