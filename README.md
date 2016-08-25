# libhttpserver

## Introduction
libhttpserver allows to accept a http communication into an application.
Very simple and light, it is not fully featured, but may be useable for small project.
The content of the response are defined by a callback function.

## Build
By default the code may be integrated directly into the project.

But the Makefile builds three binary types:
 * a test application that creates a little server responding a very small HTML content.

    make

 * a static library around 12kB and a dynamic library around 80kB.

    CC=gcc make lib

libhttpserver is WIN32 compatible and can be build with mingw32:

    CC=mingw32-gcc make

and can build DLL file.

## Installation
This library is originally created to be integrated into an other project, and no installation is availlable.

But to install the library you may copy libhttpserver.so into /usr/lib and httpserver.h into /usr/include with root rights.

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
    http_server_t *server = httpserver_create(NULL, 80, 10);
    if (server)
    {
        /* add the function to response to the request */
        httpserver_addconnector(server, NULL, test_func1, NULL);
        /* start the server */
        httpserver_connect(server);
        /* wait the end of the appication */
        while (1)
            getchar();
        /* kill the server */
        httpserver_disconnect(server);
    }
