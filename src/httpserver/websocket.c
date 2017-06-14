/*****************************************************************************
 * websocket.c: Simple websocket framing support
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

#include <string.h>
#include <stdlib.h>
#ifndef WIN32
# include <netdb.h>
#else
# include <ws2tcpip.h>
#endif

#include "websocket.h"

/*********************************************************************/
struct frame_s
{

	uint8_t opcode: 4;
	uint8_t reserved: 3;
	uint8_t fin:1;

	uint8_t payloadlen: 7;
	uint8_t mask: 1;
};

enum frame_opcode_e
{
	fo_continuation,
	fo_text,
	fo_binary,
	fo_reserve3,
	fo_reserve4,
	fo_reserve5,
	fo_reserve6,
	fo_reserve7,
	fo_close,
	fo_ping,
	fo_pong,
	fo_creserveB,
	fo_creserveC,
	fo_creserveD,
	fo_creserveE,
	fo_creserveF,
};

static onclose_t _onclose;
static onping_t _onping;

void websocket_init(onclose_t onclose, onping_t onping)
{
	_onclose = onclose;
	_onping = onping;
}

int websocket_unframed(char *in, int inlength, char *out, void *arg)
{
	int i, ret = 0;
	uint8_t maskingkey[4] = {0xFF, 0xFF, 0xFF, 0xFF};
	uint64_t payloadlen = 0;
	char *payload = in;
	while (inlength > 0)
	{
		struct frame_s frame;
		frame.fin = (payload[0] & 0x80)?1:0;
		frame.opcode = (*payload) & 0x0F;
		payload++;
		frame.mask = (payload[0] & 0x80)?1:0;
		frame.payloadlen = (*payload) & 0x7F;
		payload++;
		payloadlen = frame.payloadlen;
		if (frame.payloadlen == 126)
		{
			uint32_t *more = (uint32_t *)payload;
			payloadlen = *more;
			payload += sizeof(*more);
		}
		else if (frame.payloadlen == 127)
		{
			uint64_t *more = (uint64_t *)payload;
			payloadlen += *more;
			payload += sizeof(*more);
		}
		else
			payloadlen = frame.payloadlen;
		if (frame.mask)
		{
			for (i = 0; i < 4; i++)
			{
				maskingkey[i] = payload[i];
			}
			payload += sizeof(maskingkey);
		}
		for (i = 0; i < payloadlen; i++)
		{
			out[i] = payload[i] ^ maskingkey[i % 4];
		}
		switch (frame.opcode)
		{
			case fo_text:
			{
				if (frame.fin)
				{
					out[payloadlen] = 0;
					ret++;
				}
			}
			case fo_binary:
			{
				ret += payloadlen;
			}
			break;
			case fo_close:
			{
				uint16_t status = out[0];
				status = status << 8;
				status += (out[1] & 0x00FF);
				if (frame.fin)
				{
					out[payloadlen] = 0;
				}
				if (_onclose)
					_onclose(arg, status);
			}
			break;
			case fo_ping:
			{
				if (_onping)
					_onping(arg, out);
			}
			break;
			default:
			break;
		}
		payload += payloadlen;
		inlength -= payload - in;
	}
	return ret;
}

int websocket_framed(char *in, int inlength, char *out, int *outlength, void *arg)
{
	struct frame_s frame;
	memset(&frame, 0, sizeof(frame));
/*
	if (in[inlength - 1] == 0)
	{
		frame->opcode = fo_text;
		inlength--;
	}
	else
*/
	frame.opcode = fo_text;

	frame.mask = 0;

	if (inlength < 126)
	{
		frame.payloadlen = inlength;
		frame.fin = 1;
	}
	else
	{
		frame.payloadlen = 125;
	}
	memcpy(out, &frame, sizeof(frame));
	memcpy(out + sizeof(frame), in, frame.payloadlen);
	*outlength = frame.payloadlen + sizeof(frame);
	return frame.payloadlen;
}
