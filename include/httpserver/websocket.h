/*****************************************************************************
 * websocket.h: Simple websocket framing support
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

#ifndef __WEBSOCKET_H__
#define __WEBSOCKET_H__

#define MAX_FRAGMENTHEADER_SIZE 14
typedef int (*onclose_t)(void *arg, int status);
typedef int (*onping_t)(void *arg, char *message);

#define WS_TEXT 153
struct websocket_s
{
	int type;
	int mtu;
	onclose_t onclose;
	onping_t onping;
};
typedef struct websocket_s websocket_t;

#ifdef __cplusplus
extern "C"
{
#endif

void websocket_init(websocket_t *config);
int websocket_unframed(char *in, int inlength, char *out, void *arg);
int websocket_framed(char *in, int inlength, char *out, int *outlength, void *arg);

#ifdef __cplusplus
}
#endif

#endif
