/*****************************************************************************
 * utils.h: http utils  for modules
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

#ifndef __UTILS_H__
#define __UTILS_H__

extern const char str_location[];

extern const char str_textplain[];
extern const char str_texthtml[];
extern const char str_textcss[];
extern const char str_textjson[];
extern const char str_imagepng[];
extern const char str_imagejpeg[];
extern const char str_applicationjavascript[];
extern const char str_applicationoctetstream[];

const char *utils_getmime(const char *path);
void utils_addmime(const char *ext, const char*mime);

char *utils_urldecode(const char *encoded);
int utils_searchexp(const char *haystack, const char *needleslist, const char **rest);

struct utils_parsestring_s
{
	const char *field;
	int (*cb)(void *data, const char *string, int length);
	void *cbdata;
	int result;
};
typedef struct utils_parsestring_s utils_parsestring_t;
int utils_parsestring(const char *string, int listlength, utils_parsestring_t list[]);

/**
 * @brief get value of each cookie of the request
 *
 * The function may accept NULL key in this case
 * it returns the first cookie available and
 * the next call to the function will return the next one.
 * The return is not directly the value of the cookie but
 * the key followed by the value separated by "=".
 * Example:
 * keyvalue = cookie_get(request, "foo");
 * strcmp(keyvalue, "foo=bar") == 0
 *
 * @param message the request message received
 * @param key the name of the cookie or NULL
 *
 * @return the key and value corresponding or a null pointer
 */
const char *cookie_get(http_message_t *request, const char *key);

int cookie_set(http_message_t *response, const char *key, const char *value, ...);

#endif
