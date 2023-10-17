/*****************************************************************************
 * httpserver.h: Simple HTTP server
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

#ifndef __HTTPSERVER_H__
#define __HTTPSERVER_H__

#ifndef WIN32
# include <sys/types.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <netdb.h>
#else
# include <winsock2.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef DEFAULT_MAXCLIENTS
#define DEFAULT_MAXCLIENTS 10
#endif

#define ESUCCESS 0
#define EINCOMPLETE -1
#define ECONTINUE -2
#define ESPACE -3
#define EREJECT -4
#define ETIMEOUT -5

#define EXPORT_SYMBOL __attribute__((visibility("default")))

typedef struct http_message_s http_message_t;
typedef struct http_server_s http_server_t;
typedef struct http_client_s http_client_t;

extern const char EXPORT_SYMBOL str_get[4];
extern const char EXPORT_SYMBOL str_post[5];
extern const char EXPORT_SYMBOL str_head[5];
extern const char EXPORT_SYMBOL str_defaultscheme[5];
extern const char EXPORT_SYMBOL str_form_urlencoded[34];
extern const char EXPORT_SYMBOL str_contenttype[13];
extern const char EXPORT_SYMBOL str_contentlength[15];

typedef enum
{
	HTTP09,
	HTTP10,
	HTTP11,
	HTTP20,
	HTTPVERSIONS,
	HTTPVERSION_MASK = 0x00FF,
	HTTP_PIPELINE = 0x0100,
} http_message_version_e;

EXPORT_SYMBOL int httpserver_version(http_message_version_e versionid, const char **version);

typedef int http_message_result_e;
#define RESULT_200 200
#define RESULT_400 400
#define RESULT_404 404
#define RESULT_405 405
#ifndef HTTP_STATUS_PARTIAL
#define RESULT_101 101
#define RESULT_201 201
#define RESULT_204 204
#define RESULT_206 206
#define RESULT_301 301
#define RESULT_302 302
#define RESULT_304 304
#define RESULT_307 307
#define RESULT_401 401
#define RESULT_403 403
#define RESULT_414 414
#define RESULT_416 416
#define RESULT_500 500
#define RESULT_505 505
#define RESULT_511 511
#endif

typedef void *(*http_create_t)(void *config, http_client_t *clt);
typedef int (*http_connect_t)(void *ctx, const char *addr, int port);
typedef int (*http_wait_t)(void *ctx, int options);
typedef int (*http_status_t)(void *ctx);
typedef void (*http_flush_t)(void *ctx);
/**
 * @brief callback to read the request of the client
 *
 * @param ctx          the context pointer of the module
 * @param data         the data buffer to push the request of the client
 * @param length       the size of the data buffer
 *
 * @return the length of the request of the client
 */
typedef int (*http_recv_t)(void *ctx, char *data, size_t length);
/**
 * @brief callback to send the response to the client
 *
 * @param ctx          the context pointer of the module
 * @param data         the data buffer to send
 * @param length       the size of the data buffer
 *
 * @return the length of the response
 */
typedef int (*http_send_t)(void *ctx, const char *data, size_t length);

typedef void (*http_disconnect_t)(void *ctx);
typedef void (*http_destroy_t)(void *ctx);

#define HTTPCLIENT_TYPE_SECURE 0x0001

typedef struct httpclient_ops_s httpclient_ops_t;
struct httpclient_ops_s
{
	const char *scheme;
	int default_port;
	int type;
	http_create_t create;
	http_connect_t connect; /* callback to connect on an external server */
	http_recv_t recvreq; /* callback to receive data on the socket */
	http_send_t sendresp; /* callback to send data on the socket */
	http_wait_t wait;
	http_status_t status; /* callback to get the socket status*/
	http_flush_t flush; /* callback to flush the socket */
	http_disconnect_t disconnect; /* callback to close the socket */
	http_destroy_t destroy; /* callback to close the socket */

	const httpclient_ops_t *next;
};

EXPORT_SYMBOL void httpclient_appendops(const httpclient_ops_t *ops);

/**
 * @brief callback to manage a request
 *
 * @param arg		the data associated to the callback by the http<>_addconnector function
 * @param request	the client request received
 * @param response	the server response to send after
 *
 * @return ESUCCESS	to complete the response sending, ECONTINUE to look for another callback
 *
 * The callback can set the header of the response and returns with ECONTINUE.
 * At least one callback has to return ESUCCESS otherwise the http response will be "404 Not found".
 * connector returns
 *  ESUCCESS for content ready and fully sent.
 *  EREJECT for request not treat by the connector.
 *  ECONTINUE for content available and more in future (exception if the
 *   content is empty, the connector needs to be called again).
 *  EINCOMPLETE for content not ready and need to be called again.
 */
typedef int (*http_connector_t)(void *arg, http_message_t *request, http_message_t *response);
#define CONNECTOR_SERVER		0
#define CONNECTOR_FILTER		1
#define CONNECTOR_AUTH			2
#define CONNECTOR_DOCFILTER		4
#define CONNECTOR_DOCUMENT		5
#define CONNECTOR_ERROR			10

/**
 * @brief callback to manage a sender module context
 *
 * @param arg		the argument passed to httpserver_addmod fucnction
 * @param clt		the client data to use with httpclient_recv and httpclient_send
 * @param addr		the client socket address
 * @param addrsize	the client socket address size
 *
 * @return the context pointer of the module
 */
typedef void *(*http_getctx_t)(void* arg, http_client_t *clt, struct sockaddr *addr, int addrsize);
/**
 * @brief callback to manage a sender module context
 *
 * @param ctx	the context pointer of the module, returned by http_getctx_t
 *
 */
typedef void (*http_freectx_t)(void *ctx);

typedef struct http_server_config_s
{
	/** @param name of the server */
	char *hostname;
	/** @param address the IP address of the network bridge to use, NULL to use ANY network bridge */
	char *addr;
	/** @param port the TCP/IP prot to bind the server */
	int port;
	/** @param service name if different to http server */
	const char *service;
	/** @param maxclients the maximum number of clients accepted by the server. */
	int maxclients;
	int chunksize;
	/** the version of the HTTP server. */
	http_message_version_e version;
	const char *versionstr;
	/** the keepalive timeout in seconds **/
	int keepalive;
} http_server_config_t;

/**
 * @brief software name
 *
 * This value may be changed, by default it is "libhttpserver".
 * It is returned by httpserver_INFO(server, "software")
 */
EXPORT_SYMBOL extern char * httpserver_software;

/**
 * @brief create a server object and open the main socket
 *
 * @param config	the server configuration structure
 *
 * @return the server object
 */
EXPORT_SYMBOL http_server_t * httpserver_create(http_server_config_t *config);

EXPORT_SYMBOL http_server_t * httpserver_dup(http_server_t *server);

/**
 * @brief get value for different attributs of the server
 *
 * keys supported:
 *  - name, host, hostname
 *  - software
 *  - scheme
 *  - port
 *  - protocol
 *  - addr
 *
 * @param server the server object generated by httpserver_create
 * the standard attributs received are:
 *  - name : the host name defined at the configuration
 *  - software : the name of the application
 *  - protocol : the protocol type ("HTTP/1.1") received
 *  - connection : the connection type requested ("Keep-alive")
 *  - date : the date of the request
 *  and any other header attribut of the request message
 * @param key the name of the attribut
 *
 * @return the value of the attribut or a empty string
 */

EXPORT_SYMBOL const char * httpserver_INFO(http_server_t *server, const char *key);

/**
 * @brief get value for different attributs of the server as httpserver_INFO
 *
 * @param server the server object generated by httpserver_create
 * @param key the name of the attribut
 * @param value the pointer to set
 *
 * @return the length ogf value, -1 to use strlen, 0 if empty
 */

EXPORT_SYMBOL size_t httpserver_INFO2(http_server_t *server, const char *key, const char **value);

/**
 * @brief add a HTTP method to manage
 *
 * @param server the server object generated by httpserver_create
 * @param method the method string to managae ("GET", "HEAD", "PUT"...)
 * @param length the length of the method string
 * @param properties a value checked by some modules
 * @note by default server knows "GET", "POST" and "HEAD"
 */
#define MESSAGE_PROTECTED 0x01
#define MESSAGE_ALLOW_CONTENT 0x02
#define METHOD(string) string, sizeof(string) - 1
EXPORT_SYMBOL void httpserver_addmethod(http_server_t *server, const char *method, size_t length, short properties);

/**
 * @brief add a HTTP method to manage
 *
 * @param server the server object generated by httpserver_create
 * @param newops the protocol to use with new client connection
 * @param config the first argument of create function.
 * @return the previous protocol
 */
EXPORT_SYMBOL const httpclient_ops_t * httpserver_changeprotocol(http_server_t *server, const httpclient_ops_t *newops, void *config);

/**
 * @brief add a callback on client message reception
 *
 * @param server the server object generated by httpserver_create
 * @param func the callback to callback
 * @param data the first parameter to send to the callback
 * @param priority the level to order the connectors
 * @param name the name of the module which add the connector
 */
EXPORT_SYMBOL void httpserver_addconnector(http_server_t *server, http_connector_t func, void *data, int priority, const char *name);

/**
 * @brief restart the connectors from client with other server's connectors
 *
 * This is usefull for virtual server
 *
 * @param server the server object generated by httpserver_create
 * @param client the client to reset
 */
EXPORT_SYMBOL int httpserver_reloadclient(http_server_t *server, http_client_t *client);

/**
 * @brief add a module for client
 *
 * @param server the server object generated by httpserver_create
 * @param mod the module description
 */
EXPORT_SYMBOL void httpserver_addmod(http_server_t *server, http_getctx_t mod, http_freectx_t unmod, void *arg, const char *name);

/**
 * @brief start the server to a new thread
 *
 * @param server the server object generated by httpserver_create
 */
EXPORT_SYMBOL void httpserver_connect(http_server_t *server);

/**
 * @brief run all servers
 *
 * @param server the first server connected
 *
 * @return ESUCCESS when all servers closed
 * 	EINCOMPLETE when at least one server must be call again.
 */
EXPORT_SYMBOL int httpserver_run(http_server_t *server);

/**
 * @brief stop the server from any thread
 *
 * @param server the server object generated by httpserver_create
 */
EXPORT_SYMBOL void httpserver_disconnect(http_server_t *server);

/**
 * @brief destroy the server object
 *
 * @param server the server object generated by httpserver_create
 */
EXPORT_SYMBOL void httpserver_destroy(http_server_t *server);

/*****************************************/
/** internal functions for the callback **/
/*****************************************/
/**
 * @brief create message
 *
 * @return the message
 *
 * create message to be use with parsecgi
 * out of modules
 *
 * This function is available only if HTTPCLIENT_FEATURES is defined
 */
EXPORT_SYMBOL http_message_t * httpmessage_create();

/**
 * @brief destroy message created with httpmessage_create
 *
 * @param message the message to destroy
 */
EXPORT_SYMBOL void httpmessage_destroy(http_message_t *message);

/**
 * @brief return the client of the request
 *
 * @param message the response message to update
 *
 * @return the client instance
 */
EXPORT_SYMBOL http_client_t * httpmessage_client(http_message_t *message);

/**
 * @brief store and return private data for callback
 *
 * @param message the response message to update
 * @param data the pointer on the data to store,
 * 	      may be null to retreive the previous storage.
 *
 * @return the same pointer as stored
 */
EXPORT_SYMBOL void * httpmessage_private(http_message_t *message, void *data);

/**
 * @brief change the result of the message
 *
 * @param message the response message to update
 * @param result value to add
 *
 * @return current result of the message.
 *
 * If result is 0, the function only returns the current result of the message.
 */
http_message_result_e EXPORT_SYMBOL httpmessage_result(http_message_t *message, http_message_result_e result);

#ifdef HTTPCLIENT_FEATURES
/**
 * @brief create a request message
 *
 * @param message the request message to update
 * @param type the method to use ("GET", "POST", "HEAD"...)
 * @param resource the path parts of the URI
 * @param ... other parts of the URI
 *
 * @return the client to use for sending
 * This function is available only if HTTPCLIENT_FEATURES is defined
 */
EXPORT_SYMBOL http_client_t * httpmessage_request(http_message_t *message, const char *method, const char *resource, ...);
#endif

/**
 * @brief add a header to the response message
 *
 * @param message the response message to update
 * @param key the name of the header to set
 * @param value the value to set
 * @param valuelen the length of previous string
 *
 * @result ESUCCESS or EREJECT if not enough memory
 */
EXPORT_SYMBOL int httpmessage_addheader(http_message_t *message, const char *key, const char *value, ssize_t valuelen);

/**
 * @brief apend the last header of the response message
 *
 * @param message the response message to update
 * @param key the name of the last header to set
 * @param value several const char * string terminate with a NULL pointer
 */
EXPORT_SYMBOL int httpmessage_appendheader(http_message_t *message, const char *key, const char *value, ssize_t valuelen);

/**
 * @brief add the content to the response message
 *
 * @param message the response message to update
 * @param type the mime type of the content, NULL will set to "text/plain"
 * @param content the data of the content
 * @param length the length of the bitstream of the content
 *
 * @return the space available into the chunk of content
 */
EXPORT_SYMBOL int httpmessage_addcontent(http_message_t *message, const char *type, const char *content, int length);

/**
 * @brief append data to content of the response message before sending
 *
 * @param message the response message to update
 * @param content the data of the content
 * @param length the length of the bitstream of the content
 *
 * @return the space available into the chunk of content
 */
EXPORT_SYMBOL int httpmessage_appendcontent(http_message_t *message, const char *content, int length);

/**
 * @brief returns the content of the request message
 *
 * @param message the request message
 * @param contentpart the data of the content
 * @param contentlenght the rest size of the content to read
 *
 * @return the length of data pop into contentpart
 */
EXPORT_SYMBOL int httpmessage_content(http_message_t *message, const char **contentpart, size_t *contentlenght);

/**
 * @brief set the Keep-Alive connection
 *
 * the server will keep the client connection opened
 * any thread can use this connection to send more data as content
 *
 * @param message the response message to update
 *
 * @return the client socket descriptor
 */
EXPORT_SYMBOL int httpmessage_keepalive(http_message_t *message);

/**
 * @brief lock the connection
 *
 * the server will keep the client connection opened
 * any thread can use this connection to send more data directly
 * onto the socket.
 *
 * @param message the response message to update
 *
 * @return the client socket descriptor
 */
EXPORT_SYMBOL int httpmessage_lock(http_message_t *message);

/**
 * @brief return the protection of the message
 *
 * if the message is protected, an authentication should be done.
 *
 * @param message the request to test
 *
 * @return 0 for unprotected message, or -1 for forbidden message,
 * otherwise a value > 0
 */
EXPORT_SYMBOL int httpmessage_isprotected(http_message_t *message);

/**
 * @brief create response for data stream
 *
 * @param message the response message to update
 * @param data the stream to parse
 * @param size the length of the data and return the new length if changed
 *
 * @return ECONTINUE on the need of more data
 *         ESUCCESS when a response with content is ready
 *
 * After ESUCCESS, it is possible to add more data into the content
 * with this same function.
 *
 * This function is available only if HTTPCLIENT_FEATURES is defined
 */
EXPORT_SYMBOL int httpmessage_parsecgi(http_message_t *message, char *data, int *size);

/**
 * @brief get value for different attributs of the connection
 *
 * the standard attributs received are:
 *  - name : the host name defined at the configuration
 *  - software : the name of the application
 *  - protocol : the protocol type ("HTTP/1.1") received
 *  - connection : the connection type requested ("Keep-alive")
 *  - date : the date of the request
 *  and any other header attribut of the request message
 *
 * @param message the request message received
 * @param key the name of the attribut
 *
 * @return the value of the attribut or a empty string
 */

EXPORT_SYMBOL const char * httpmessage_SERVER(http_message_t *message, const char *key);

/**
 * @brief get value for different attributs of the request
 *
 * the value can come from GET or POST data
 *
 * @param message the request message received
 * @param key the name of the attribut
 *
 * @return the value of the attribut or a empty string
 */
EXPORT_SYMBOL const char * httpmessage_REQUEST(http_message_t *message, const char *key);
EXPORT_SYMBOL int httpmessage_REQUEST2(http_message_t *message, const char *key, const char **value);

/**
 * @brief get/set value for the session used by the request
 *
 * the value are stored by mod with the same function.
 *
 * @param message the request message received
 * @param key the name of the attribut
 * @param value the value to set with the key or NULL
 * @param size the size of value
 *
 * @return the value of the attribute or NULL
 */
EXPORT_SYMBOL const void *httpmessage_SESSION(http_message_t *message, const char *key, void *value, int size) __attribute__ ((deprecated));

/**
 * @brief get value for the session used by the request
 *
 * the value are stored by mod with the same function.
 *
 * @param message the request message received
 * @param key the name of the attribut
 * @param value the pointer to return the content
 *
 * @return the length of the content
 */
EXPORT_SYMBOL size_t httpmessage_SESSION2(http_message_t *message, const char *key, void **value);

/**
 * @brief get value from query parameters and/or POST form data
 *
 * the value are stored by mod with the same function.
 *
 * @param message the request message received
 * @param key the name of the attribute
 *
 * @return the value of the paramater or NULL
 */
EXPORT_SYMBOL const char * httpmessage_parameter(http_message_t *message, const char *key);

/**
 * @brief get value from Cookie header
 *
 * the value are stored by mod with the same function.
 *
 * @param message the request message received
 * @param key the name of the attribute
 * @param cookie the pointer to set to the data
 *
 * @return the length of the data
 */
EXPORT_SYMBOL size_t httpmessage_cookie(http_message_t *message, const char *key, const char **cookie);

/**********************************************************************/
typedef struct httpclient_ops_s httpclient_ops_t;
/**
 * @brief create a new client for the server or a client application
 *
 * @param server the server which manage the client or NULL for client application
 * @param fops the collection of operation to use client socket (tcpclient_ops)
 *
 * @return the new client on success or NULL on error
 *
 * This function is available only if HTTPCLIENT_FEATURES is defined
 */
EXPORT_SYMBOL http_client_t * httpclient_create(http_server_t *server, const httpclient_ops_t *fops, void *protocol);
EXPORT_SYMBOL extern const httpclient_ops_t * tcpclient_ops;

/**
 * @brief delete the client object
 *
 * @param client the connection that will send the request
 */
void EXPORT_SYMBOL httpclient_destroy(http_client_t *client);

/**
 * @brief create a session for this client
 * 
 * @param client the connection that receive the request
 * @param token the token to assign to the session
 * 
 * @return EREJECT if the client already has a session otherwise ESUCCESS
 */
EXPORT_SYMBOL int httpclient_setsession(http_client_t *client, const char *token, size_t tokenlen);

/**
 * @brief get/set session DB
 * 
 * @param client the connection that receive the request
 * @param key the key into the DB to get or to set
 * @param value the value to set for the key into the DB or NULL to get it
 * @param size the length of the value to store (-1 for null terminated string)
 * 
 * @return the value found into the DB or NULL
 */
EXPORT_SYMBOL const void *httpclient_session(http_client_t *client, const char *key, size_t keylen, const void *value, size_t size);

/**
 * @brief set session DB
 * 
 * @param client the connection that receive the request
 * @param key the key must be the same as the previous httpclient_session call.
 * @param value the value to append
 * @param size the length of the value to store (-1 for null terminated string)
 * 
 * @return the value found into the DB or NULL
 */
EXPORT_SYMBOL const void *httpclient_appendsession(http_client_t *client, const char *key, const void *value, size_t size);

/**
 * @brief delete session DB
 * 
 * @param client the connection that receive the request
 * 
 */
EXPORT_SYMBOL void httpclient_dropsession(http_client_t *client);

#ifdef HTTPCLIENT_FEATURES
/**
 * @brief send a request with client
 *
 * @param client the connection that will send the request
 * @param request the request
 * @param response the response
 *
 * @return ESUCCESS on success EREJECT on error
 *
 * This function is available only if HTTPCLIENT_FEATURES is defined
 */
EXPORT_SYMBOL int httpclient_sendrequest(http_client_t *client, http_message_t *request, http_message_t *response);
#endif

/**
 * @brief return the receiver and sender context
 *
 * @param client the connection that is receiving the request
 *
 * @return return the context
 */
EXPORT_SYMBOL void * httpclient_context(http_client_t *client);

/**
 * @brief return the socket descriptor
 *
 * @param client the connection that is receiving the request
 *
 * @return return the socket
 */
EXPORT_SYMBOL int httpclient_socket(http_client_t *client);

/**
 * @brief return the server which has created this client
 *
 * @param client the connection that is receiving the request
 *
 * @return return the server
 */
EXPORT_SYMBOL http_server_t * httpclient_server(http_client_t *client);

/**
 * @brief add a callback on client message reception
 *
 * @param client the connection that is receiving the request
 * @param vhost the HOST name on which the connector has to response, NULL for default host
 * @param func the callback to callback
 * @param funcarg the first parameter to send to the callback
 * @param priority the level to order the connectors
 * @param name the name of the module which add the connector
 */

EXPORT_SYMBOL void httpclient_addconnector(http_client_t *client, http_connector_t func, void *funcarg, int priority, const char *name);

/**
 * @brief add a request receiver callback
 *
 * @param client the connection that is receiving the request
 * @param func the callback
 * @param arg the first parameter of the callback
 *
 * @return return the previous request receiver callback
 */
EXPORT_SYMBOL http_recv_t httpclient_addreceiver(http_client_t *client, http_recv_t func, void *arg);

/**
 * @brief add a response sender callback
 *
 * @param client the connection that received the request
 * @param func the callback
 * @param arg the first parameter of the callback
 *
 * @return return the previous response sender callback
 */
EXPORT_SYMBOL http_send_t httpclient_addsender(http_client_t *client, http_send_t func, void *arg);


/**
 * @brief shutdown and close the client
 *
 * @param client the connection that received the request
 */
EXPORT_SYMBOL void httpclient_shutdown(http_client_t *client);

/**
 * @brief wait on the socket while no dat available
 *
 * @param client the connection that received the request
 * @param options 1 to wait place to send data, 0 to receive
 *
 * @return socket fd
 */
#define WAIT_SEND 0x01
#define WAIT_ACCEPT 0x02
EXPORT_SYMBOL int httpclient_wait(http_client_t *client, int options);

/**
 * @brief flush the data on the socket
 *
 * @param client the connection that received the request
 *
 */
EXPORT_SYMBOL void httpclient_flush(http_client_t *client);

#ifdef __cplusplus
}
#endif

#endif
