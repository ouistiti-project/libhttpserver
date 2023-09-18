/*****************************************************************************
 * _httpmessage.h: HTTP message private data
 *****************************************************************************
 * Copyright (C) 2016-2017
 *
 * Authors: Marc Chalain <marc.chalain@gmail.com
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

#ifndef ___HTTPMESSAGE_H__
#define ___HTTPMESSAGE_H__

#ifndef WIN32
# include <sys/un.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <netdb.h>
#else
# include <winsock2.h>
#endif

#include "dbentry.h"
#include "_string.h"

#define HTTPMESSAGE_KEEPALIVE 0x01
#define HTTPMESSAGE_LOCKED 0x02

extern const char str_true[];
extern const char str_get[];
extern const char str_post[];
extern const char str_head[];
extern const char str_form_urlencoded[];
extern const char str_cookie[];
extern const char str_connection[];
extern const char str_contenttype[];
extern const char str_contentlength[];

typedef struct buffer_s buffer_t;

typedef struct http_connector_list_s http_connector_list_t;
struct http_connector_list_s
{
	http_connector_t func;
	void *arg;
	struct http_connector_list_s *next;
	const char *name;
	int priority;
};
void _httpconnector_add(http_connector_list_t **first,
						http_connector_t func, void *funcarg,
						int priority, const char *name);

struct http_message_s
{
	http_message_result_e result;
	int mode;
	http_client_t *client;
	http_message_t *response;
	void *connector;
	const http_message_method_t *method;
	enum {
		PARSE_INIT,
		PARSE_URI = 0x0001,
		PARSE_QUERY = 0x0002,
		PARSE_URIDOUBLEDOT = 0x0003,
		PARSE_VERSION = 0x0004,
		PARSE_STATUS = 0x0005,
		PARSE_PREHEADER = 0x0006,
		PARSE_HEADER = 0x0007,
		PARSE_POSTCONTENT = 0x0008, /**POSTCONTENT is here to allow to parse all the content of POST request  see _httpclient_request*/
		PARSE_POSTHEADER = 0x0009,
		PARSE_PRECONTENT = 0x000A,
		PARSE_CONTENT = 0x000B,
		PARSE_END = 0x000C,
		PARSE_MASK = 0x000F,
		GENERATE_ERROR = 0x0010,
		GENERATE_INIT = 0x0020,
		GENERATE_RESULT = 0x0030,
		GENERATE_HEADER = 0x0040,
		GENERATE_SEPARATOR = 0x0050,
		GENERATE_CONTENT = 0x0060,
		GENERATE_END = 0x00F0,
		GENERATE_MASK = 0x00F0,
		PARSE_CONTINUE = 0x0100,
	}
	state;
	buffer_t *content;
	buffer_t *content_storage;
	buffer_t *header;
	unsigned long long content_length;
	unsigned int content_packet;
	const char *content_type;
	buffer_t *uri;
	http_message_version_e version;
	buffer_t *headers_storage;
	dbentry_t *headers;
	buffer_t *query_storage;
	dbentry_t *queries;
	buffer_t *cookie_storage;
	dbentry_t *cookies;
	void *private;
	http_message_t *next;
	char decodeval;
};

struct _http_message_result_s
{
	int result;
	string_t status;
};
typedef struct _http_message_result_s _http_message_result_t;

extern const _http_message_result_t *_http_message_result[];

typedef struct http_message_method_s http_message_method_t;
struct http_message_method_s
{
	string_t key;
	short id;
	short properties;
	http_message_method_t *next;
};

extern const http_message_method_t default_methods[];

typedef enum
{
	MESSAGE_TYPE_GET,
	MESSAGE_TYPE_POST,
	MESSAGE_TYPE_HEAD,
} _http_message_method_e;


http_message_t * _httpmessage_create(http_client_t *client, http_message_t *parent);
void _httpmessage_destroy(http_message_t *message);
int _httpmessage_buildresponse(http_message_t *message, int version, buffer_t *header);
buffer_t *_httpmessage_buildheader(http_message_t *message);
int _httpmessage_parserequest(http_message_t *message, buffer_t *data);
int _httpmessage_fillheaderdb(http_message_t *message);
size_t _httpmessage_status(const http_message_t *message, char *status, size_t statuslen);
int _httpmessage_changestate(http_message_t *message, int new);
int _httpmessage_state(http_message_t *message, int check);
int _httpmessage_contentempty(http_message_t *message, int unset);
int _httpmessage_runconnector(http_message_t *request, http_message_t *response);

#define _HTTPMESSAGE_RESULT_DEFINE(_id, _status) &(_http_message_result_t){.result = _id, .status.data = _status, .status.length = sizeof(_status) - 1}
#define _HTTPMESSAGE_RESULT_MAXLEN 40

#ifdef _HTTPMESSAGE_
const _http_message_result_t *_http_message_result[] =
{
#if defined(RESULT_100)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_100, " 100 Continue"),
#endif
#if defined(RESULT_101)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_101, " 101 Switching Protocols"),
#endif
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_200, " 200 OK"),
#if defined(RESULT_201)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_201, " 201 Created"),
#endif
#if defined(RESULT_202)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_202, " 202 Accepted"),
#endif
#if defined(RESULT_203)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_203, " 203 Non-Authoritative Information"),
#endif
#if defined(RESULT_204)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_204, " 204 No Content"),
#endif
#if defined(RESULT_205)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_205, " 205 Reset Content"),
#endif
#if defined(RESULT_206)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_206, " 206 Partial Content"),
#endif
#if defined(RESULT_300)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_300, " 300 Multiple Choices"),
#endif
#if defined(RESULT_301)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_301, " 301 Moved Permanently"),
#endif
#if defined(RESULT_302)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_302, " 302 Found"),
#endif
#if defined(RESULT_303)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_303, " 303 See Other"),
#endif
#if defined(RESULT_304)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_304, " 304 Not Modified"),
#endif
#if defined(RESULT_305)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_305, " 305 Use Proxy"),
#endif
#if defined(RESULT_307)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_307, " 307 Temporary Redirect"),
#endif
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_400, " 400 Bad Request"),
#if defined(RESULT_401)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_401, " 401 Unauthorized"),
#endif
#if defined(RESULT_402)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_402, " 402 Payment Required"),
#endif
#if defined(RESULT_403)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_403, " 403 Forbidden"),
#endif
#if defined(RESULT_404)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_404, " 404 File Not Found"),
#endif
#if defined(RESULT_405)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_405, " 405 Method Not Allowed"),
#endif
#if defined(RESULT_406)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_406, " 406 Not Acceptable"),
#endif
#if defined(RESULT_407)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_407, " 407 Proxy Authentication Required"),
#endif
#if defined(RESULT_408)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_408, " 408 Request Timeout"),
#endif
#if defined(RESULT_409)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_409, " 409 Conflict"),
#endif
#if defined(RESULT_410)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_410, " 410 Gone"),
#endif
#if defined(RESULT_411)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_411, " 411 Length Required"),
#endif
#if defined(RESULT_412)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_412, " 412 Precondition Failed"),
#endif
#if defined(RESULT_413)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_413, " 413 Request Entity Too Large"),
#endif
#if defined(RESULT_414)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_414, " 414 Request URI too long"),
#endif
#if defined(RESULT_415)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_415, " 415 Unsupported Media Type"),
#endif
#if defined(RESULT_416)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_416, " 416 Range Not Satisfiable"),
#endif
#if defined(RESULT_417)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_417, " 417 Expectation Failed"),
#endif
#if defined(RESULT_500)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_500, " 500 Internal Server Error"),
#endif
#if defined(RESULT_501)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_501, " 501 Not Implemented"),
#endif
#if defined(RESULT_502)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_502, " 502 Bad Gateway"),
#endif
#if defined(RESULT_503)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_503, " 503 Service Unavailable"),
#endif
#if defined(RESULT_504)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_504, " 504 Gateway Timeout"),
#endif
#if defined(RESULT_505)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_505, " 505 HTTP Version Not Supported"),
#endif
#if defined(RESULT_506)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_506, " 506 Variant Also Negotiates"),
#endif
#if defined(RESULT_511)
	_HTTPMESSAGE_RESULT_DEFINE(RESULT_511, " 511 Network Authentication Required"),
#endif
	NULL
};

#endif
#endif

