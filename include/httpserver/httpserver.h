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

/**
 * MAXCHUNKS defines the maximum number of memory chunk which may be allocated
 * The size of the chunks is configurable with the server (see chunksize).
 * 
 * The header may be large in some cases like POST multipart/form-data messages.
 * But it could be an attack by memory overflow. The value of MAXCHUNKS_HEADER
 * has to be correctly set (4 is to small, 8 seems to large for embeded target)
 *
 * The content may be larger than 3 chunks. But httpserver send chunk by chunk
 * the content. It may exist one case, it is a module (not of the currently modules
 * available) which want to way the end of the content before to send.
 * This may be done with a connector which returns EINCOMPLETE.
 * If a new module uses this feature and needs more than 3 chunk before
 * to send, the value MAXCHUNKS_CONTENT has to be increased.
 */
#ifndef MAXCHUNKS_HEADER
#define MAXCHUNKS_HEADER  8
#endif
#ifndef MAXCHUNKS_CONTENT
#define MAXCHUNKS_CONTENT 3
#endif
#ifndef MAXCHUNKS_SESSION
#define MAXCHUNKS_SESSION 2
#endif
#ifndef MAXCHUNKS_URI
#define MAXCHUNKS_URI 2
#endif

#define ESUCCESS 0
#define EINCOMPLETE -1
#define ECONTINUE -2
#define ESPACE -3
#define EREJECT -4
#define ETIMEOUT -5

typedef struct http_message_s http_message_t;
typedef struct http_server_s http_server_t;
typedef struct http_client_s http_client_t;

extern const char *str_get;
extern const char *str_post;
extern const char *str_head;

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

typedef int http_message_result_e;
#define RESULT_200 200
#define RESULT_400 400
#define RESULT_404 404
#define RESULT_405 405
#ifndef HTTP_STATUS_PARTIAL
#define RESULT_101 101
#define RESULT_206 206
#define RESULT_301 301
#define RESULT_302 302
#define RESULT_304 304
#define RESULT_401 401
#define RESULT_403 403
#define RESULT_414 414
#define RESULT_416 416
#define RESULT_505 505
#define RESULT_511 511
#endif

/**
 * @brief callback to manage a request
 * 
 * @param arg		the pointer given to httpserver_addconnector
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
/**
 * @brief callback to read the request of the client
 * 
 * @param ctx		the context pointer of the module
 * @param data		the data buffer to push the request of the client
 * @param length	the size of the data buffer
 * 
 * @return the length of the request of the client
 */
typedef int (*http_recv_t)(void *ctx, char *data, int length);
/**
 * @brief callback to send the response to the client
 * 
 * @param ctx		the context pointer of the module
 * @param data		the data buffer to send
 * @param length	the size of the data buffer
 * 
 * @return the length of the response
 */
typedef int (*http_send_t)(void *ctx, char *data, int length);

typedef struct http_server_config_s
{
	/** @param name of the server */
	char *hostname;
	/** @param address the IP address of the network bridge to use, NULL to use ANY network bridge */
	char *addr;
	/** @param port the TCP/IP prot to bind the server */
	int port;
	/** @param maxclients the maximum number of clients accepted by the server. */
	int maxclients;
	int chunksize;
	/** the version of the HTTP server. */
	http_message_version_e version;
	/** the keepalive timeout **/
	int keepalive;
} http_server_config_t;

/**
 * @brief software name
 * 
 * This value may be changed, by default it is "libhttpserver".
 * It is returned by httpserver_INFO(server, "software")
 */
extern char *httpserver_software;

/**
 * @brief create a server object and open the main socket
 *
 * @param config	the server configuration structure
 *
 * @return the server object
 */
http_server_t *httpserver_create(http_server_config_t *config);

/**
 * @brief get value for different attributs of the server
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

char *httpserver_INFO(http_server_t *server, const char *key);

/**
 * @brief add a HTTP method to manage
 *
 * @param server the server object generated by httpserver_create
 * @param method the method string to managae ("GET", "HEAD", "PUT"...)
 * @param properties a value checked by some modules
 * @note by default server knows "GET", "POST" and "HEAD"
 */
void httpserver_addmethod(http_server_t *server, const char *method, short properties);

/**
 * @brief add a callback on client message reception
 *
 * @param server the server object generated by httpserver_create
 * @param vhost the HOST name on which the connector has to response, NULL for default host
 * @param func the callback to callback
 * @param funcarg the first parameter to send to the callback
 */
void httpserver_addconnector(http_server_t *server, char *vhost, http_connector_t func, void *funcarg);

/**
 * @brief add a module for client
 *
 * @param server the server object generated by httpserver_create
 * @param mod the module description
 */
void httpserver_addmod(http_server_t *server, http_getctx_t mod, http_freectx_t unmod, void *arg, const char *name);

/**
 * @brief start the server to a new thread
 *
 * @param server the server object generated by httpserver_create
 */
void httpserver_connect(http_server_t *server);

/**
 * @brief stop the server from any thread
 *
 * @param server the server object generated by httpserver_create
 */
void httpserver_disconnect(http_server_t *server);

/**
 * @brief destroy the server object
 *
 * @param server the server object generated by httpserver_create
 */
void httpserver_destroy(http_server_t *server);

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
http_message_t * httpmessage_create(int chunksize);

/**
 * @brief destroy message created with httpmessage_create
 * 
 * @param message the message to destroy
 */
void httpmessage_destroy(http_message_t *message);

/**
 * @brief return the client of the request
 *
 * @param message the response message to update
 * 
 * @return the client instance
 */
http_client_t *httpmessage_client(http_message_t *message);

/**
 * @brief store and return private data for callback
 *
 * @param message the response message to update
 * @param data the pointer on the data to store, 
 * 	      may be null to retreive the previous storage.
 * 
 * @return the same pointer as stored
 */
void *httpmessage_private(http_message_t *message, void *data);

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
http_message_result_e httpmessage_result(http_message_t *message, http_message_result_e result);

/**
 * @brief create a request message
 *
 * @param message the request message to update
 * @param type the method to use ("GET", "POST", "HEAD"...)
 * @param resource the path+query parts of the URI
 * 
 * This function is available only if HTTPCLIENT_FEATURES is defined
 */
void httpmessage_request(http_message_t *message, const char *method, char *resource);

/**
 * @brief add a header to the response message
 *
 * @param message the response message to update
 * @param key the name of the header to set
 * @param value the value to set
 */
void httpmessage_addheader(http_message_t *message, const char *key, char *value);

/**
 * @brief add the content to the response message
 *
 * @param message the response message to update
 * @param type the mime type of the content, NULL will set to "text/plain"
 * @param content the data of the content
 * @param length the length of the bitstream of the content
 * 
 * @return the storage pointer of the content
 */
char *httpmessage_addcontent(http_message_t *message, char *type, char *content, int length);

/**
 * @brief append data to content of the response message before sending
 *
 * @param message the response message to update
 * @param content the data of the content
 * @param length the length of the bitstream of the content
 * 
 * @return the storage pointer of the content
 */
char *httpmessage_appendcontent(http_message_t *message, char *content, int length);

/**
 * @brief returns the content of the request message
 *
 * @param message the request message
 * @param content the data of the content
 * @param length the length of the bitstream of the content
 * 
 * @return the length of the content not already read
 */
int httpmessage_content(http_message_t *message, char **content, int *length);

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
int httpmessage_keepalive(http_message_t *message);

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
int httpmessage_lock(http_message_t *message);

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
int httpmessage_isprotected(http_message_t *message);

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
int httpmessage_parsecgi(http_message_t *message, char *data, int *size);

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

char *httpmessage_SERVER(http_message_t *message, const char *key);

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
char *httpmessage_REQUEST(http_message_t *message, const char *key);

/**
 * @brief get value of each cookie of the request
 *
 * @param message the request message received
 * @param key the name of the cookie
 *
 * @return the value of the attribut or a null pointer
 */
char *httpmessage_COOKIE(http_message_t *message, const char *key);

/**
 * @brief get value for the session used by the request
 *
 * the value are stored by mod with the same function.
 *
 * @param message the request message received
 * @param key the name of the attribut
 * @param value the value to set with the key or NULL
 *
 * @return the value of the attribut or a empty string
 */
char *httpmessage_SESSION(http_message_t *message, const char *key, char *value);

/**********************************************************************/
typedef struct httpclient_ops_s httpclient_ops_t;
/**
 * @brief create a new client for the server or a client application
 *
 * @param server the server which manage the client or NULL for client application
 * @param fops the collection of operation to use client socket (tcpclient_ops)
 * @param chunksize the size of chunked data to collect on the socket
 *
 * @return the new client on success or NULL on error
 *
 * This function is available only if HTTPCLIENT_FEATURES is defined
 */
http_client_t *httpclient_create(http_server_t *server, httpclient_ops_t *fops, int chunksize);
extern httpclient_ops_t *tcpclient_ops;

/**
 * @brief delete the client object
 * 
 * @param client the connection that will send the request
 */
void httpclient_destroy(http_client_t *client);

/**
 * @brief connect the client to an external server
 *
 * @param client the connection that will send the request
 * @param addr the address of the server
 * @param port the port of the server
 *
 * @return ESUCCESS on success EREJECT on error
 *
 * This function is available only if HTTPCLIENT_FEATURES is defined
 */
int httpclient_connect(http_client_t *client, char *addr, int port);

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
int httpclient_sendrequest(http_client_t *client, http_message_t *request, http_message_t *response);

/**
 * @brief return the receiver and sender context
 *
 * @param client the connection that is receiving the request
 *
 * @return return the context
 */
void *httpclient_context(http_client_t *client);

/**
 * @brief add a callback on client message reception
 *
 * @param client the connection that is receiving the request
 * @param vhost the HOST name on which the connector has to response, NULL for default host
 * @param func the callback to callback
 * @param funcarg the first parameter to send to the callback
 * @param name the name of the module which add the connector
 */

void httpclient_addconnector(http_client_t *client, char *vhost, http_connector_t func, void *funcarg, const char *name);

/**
 * @brief add a request receiver callback
 *
 * @param client the connection that is receiving the request
 * @param func the callback
 * @param arg the first parameter of the callback
 *
 * @return return the previous request receiver callback
 */
http_recv_t httpclient_addreceiver(http_client_t *client, http_recv_t func, void *arg);

/**
 * @brief add a response sender callback
 *
 * @param client the connection that received the request
 * @param func the callback
 * @param arg the first parameter of the callback
 *
 * @return return the previous response sender callback
 */
http_send_t httpclient_addsender(http_client_t *client, http_send_t func, void *arg);


/**
 * @brief shutdown and close the client
 *
 * @param client the connection that received the request
 */
void httpclient_shutdown(http_client_t *client);

/**
 * @brief wait on the socket while no dat available
 * 
 * @param client the connection that received the request
 * @param sending 1 to wait place to send data, 0 to receive
 * 
 * @return socket fd
 */
int httpclient_wait(http_client_t *client, int sending);
#ifdef __cplusplus
}
#endif

#endif
