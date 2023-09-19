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
		PARSE_URIENCODED = 0x0002,
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
	char *query;
	buffer_t *query_storage;
	dbentry_t *queries;
	const char *cookie;
	buffer_t *cookie_storage;
	dbentry_t *cookies;
	void *private;
	http_message_t *next;
	char decodeval;
};

struct _http_message_result_s
{
	int result;
	char *status;
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
char *_httpmessage_status(http_message_t *message);
int _httpmessage_changestate(http_message_t *message, int new);
int _httpmessage_state(http_message_t *message, int check);
int _httpmessage_contentempty(http_message_t *message, int unset);
int _httpmessage_runconnector(http_message_t *request, http_message_t *response);

#ifdef _HTTPMESSAGE_
const _http_message_result_t *_http_message_result[] =
{
#if defined(RESULT_100)
	&(_http_message_result_t){.result = RESULT_100, .status = " 100 Continue"},
#endif
#if defined(RESULT_101)
	&(_http_message_result_t){.result = RESULT_101, .status = " 101 Switching Protocols"},
#endif
	&(_http_message_result_t){.result = RESULT_200, .status = " 200 OK"},
#if defined(RESULT_201)
	&(_http_message_result_t){.result = RESULT_201, .status = " 201 Created"},
#endif
#if defined(RESULT_202)
	&(_http_message_result_t){RESULT_202, .status = " 202 Accepted"},
#endif
#if defined(RESULT_203)
	&(_http_message_result_t){.result = RESULT_203, .status = " 203 Non-Authoritative Information"},
#endif
#if defined(RESULT_204)
	&(_http_message_result_t){.result = RESULT_204, .status = " 204 No Content"},
#endif
#if defined(RESULT_205)
	&(_http_message_result_t){.result = RESULT_205, .status = " 205 Reset Content"},
#endif
#if defined(RESULT_206)
	&(_http_message_result_t){.result = RESULT_206, .status = " 206 Partial Content"},
#endif
#if defined(RESULT_300)
	&(_http_message_result_t){.result = RESULT_300, .status = " 300 Multiple Choices"},
#endif
#if defined(RESULT_301)
	&(_http_message_result_t){.result = RESULT_301, .status = " 301 Moved Permanently"},
#endif
#if defined(RESULT_302)
	&(_http_message_result_t){.result = RESULT_302, .status = " 302 Found"},
#endif
#if defined(RESULT_303)
	&(_http_message_result_t){.result = RESULT_303, .status = " 303 See Other"},
#endif
#if defined(RESULT_304)
	&(_http_message_result_t){.result = RESULT_304, .status = " 304 Not Modified"},
#endif
#if defined(RESULT_305)
	&(_http_message_result_t){.result = RESULT_305, .status = " 305 Use Proxy"},
#endif
#if defined(RESULT_307)
	&(_http_message_result_t){.result = RESULT_307, .status = " 307 Temporary Redirect"},
#endif
	&(_http_message_result_t){.result = RESULT_400, .status = " 400 Bad Request"},
#if defined(RESULT_401)
	&(_http_message_result_t){.result = RESULT_401, .status = " 401 Unauthorized"},
#endif
#if defined(RESULT_402)
	&(_http_message_result_t){.result = RESULT_402, .status = " 402 Payment Required"},
#endif
#if defined(RESULT_403)
	&(_http_message_result_t){.result = RESULT_403, .status = " 403 Forbidden"},
#endif
#if defined(RESULT_404)
	&(_http_message_result_t){.result = RESULT_404, .status = " 404 File Not Found"},
#endif
#if defined(RESULT_405)
	&(_http_message_result_t){.result = RESULT_405, .status = " 405 Method Not Allowed"},
#endif
#if defined(RESULT_406)
	&(_http_message_result_t){.result = RESULT_406, .status = " 406 Not Acceptable"},
#endif
#if defined(RESULT_407)
	&(_http_message_result_t){.result = RESULT_407, .status = " 407 Proxy Authentication Required"},
#endif
#if defined(RESULT_408)
	&(_http_message_result_t){.result = RESULT_408, .status = " 408 Request Timeout"},
#endif
#if defined(RESULT_409)
	&(_http_message_result_t){.result = RESULT_409, .status = " 409 Conflict"},
#endif
#if defined(RESULT_410)
	&(_http_message_result_t){.result = RESULT_410, .status = " 410 Gone"},
#endif
#if defined(RESULT_411)
	&(_http_message_result_t){.result = RESULT_411, .status = " 411 Length Required"},
#endif
#if defined(RESULT_412)
	&(_http_message_result_t){.result = RESULT_412, .status = " 412 Precondition Failed"},
#endif
#if defined(RESULT_413)
	&(_http_message_result_t){.result = RESULT_413, .status = " 413 Request Entity Too Large"},
#endif
#if defined(RESULT_414)
	&(_http_message_result_t){.result = RESULT_414, .status = " 414 Request URI too long"},
#endif
#if defined(RESULT_415)
	&(_http_message_result_t){.result = RESULT_415, .status = " 415 Unsupported Media Type"},
#endif
#if defined(RESULT_416)
	&(_http_message_result_t){.result = RESULT_416, .status = " 416 Range Not Satisfiable"},
#endif
#if defined(RESULT_417)
	&(_http_message_result_t){.result = RESULT_417, .status = " 417 Expectation Failed"},
#endif
#if defined(RESULT_500)
	&(_http_message_result_t){.result = RESULT_500, .status = " 500 Internal Server Error"},
#endif
#if defined(RESULT_501)
	&(_http_message_result_t){.result = RESULT_501, .status = " 501 Not Implemented"},
#endif
#if defined(RESULT_502)
	&(_http_message_result_t){.result = RESULT_502, .status = " 502 Bad Gateway"},
#endif
#if defined(RESULT_503)
	&(_http_message_result_t){.result = RESULT_503, .status = " 503 Service Unavailable"},
#endif
#if defined(RESULT_504)
	&(_http_message_result_t){.result = RESULT_504, .status = " 504 Gateway Timeout"},
#endif
#if defined(RESULT_505)
	&(_http_message_result_t){.result = RESULT_505, .status = " 505 HTTP Version Not Supported"},
#endif
#if defined(RESULT_506)
	&(_http_message_result_t){.result = RESULT_506, .status = " 506 Variant Also Negotiates"},
#endif
#if defined(RESULT_511)
	&(_http_message_result_t){.result = RESULT_511, .status = " 511 Network Authentication Required"},
#endif
	NULL
};

#endif
#endif

