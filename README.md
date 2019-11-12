# libhttpserver

## Introduction
libhttpserver allows to accept a http communication into an application.
Very simple and light, it is not fully featured, but may be useable for small project.
The content of the response are defined by a callback function.
It is also possible to create modules to link with this library to extend the features.

## Configuration
The configuration file (named "config") may be edited to set the following parameters:

 * VTHREAD=y to enable the threading support
 * VHTREAD_TYPE=[fork|pthread|win32] to set the threading type (fork is the faster)
 * MBEDTLS=y to build the SSL support with mbedTLS (previously named PolarSSL)
 * TEST=y to build the test application
 * prefix=/my/installation/path to change the installation prefix (default: /usr/local)
 * libdir=/my/libraries/path to change the installation of libraries (default: $prefix/lib)

## Build
By default the code may be integrated directly into the project.
But the Makefile builds three binary types:

 * a static library around 12kB and a dynamic library around 80kB.
> CC=gcc make

 * a test application that creates a little server responding a very small HTML content.
> make TEST=y

libhttpserver is WIN32 compatible and can be build with mingw32:
> CC=mingw32-gcc make

and can build DLL file.

## Installation

    make DESTDIR=/home/me/package install

## API
The complete API is described inside the header file.
Two examples are availlable inside the code file under the TEST flag.

    int test_func1(void *arg, http_message_t *request, http_message_t *response)
    {
    	char content[] = "<html><body>coucou<br/></body></html>";
    	/* add a special header line to the response */
    	httpmessage_addheader(response, "Server", "libhttpserver");
    	/* add a HTML content */
    	httpmessage_addcontent(response, "text/html", content, strlen(content));
    	return 0;
    }


    /* creates the server element */
    http_server_t *server = httpserver_create(NULL);
    if (server)
    {
        /* add the function to response to the request */
        httpserver_addconnector(server, test_func1, NULL);
        /* start the server */
        httpserver_connect(server);
        /* wait the end of the appication */
        while (1)
            getchar();
        /* kill the server */
        httpserver_disconnect(server);
    }
