/*
cencode.h - c header for a base64 encoding algorithm

This is part of the libb64 project, and has been placed in the public domain.
For details, see http://sourceforge.net/projects/libb64
*/

#ifndef BASE64_CENCODE_H
#define BASE64_CENCODE_H

#define __LIBB64_URLENCODING
extern int LIBB64_URLENCODING; // set this variable to 1 for urlencoding (default 0)

typedef enum
{
	step_A, step_B, step_C
} base64_encodestep;

typedef struct
{
	base64_encodestep step;
	char result;
	int stepcount;
	const char *encoding;
	char trailing_char;
} base64_encodestate;

extern const char base64_encoding_std[];
extern const char base64_encoding_url[];

void base64_init_encodestate(base64_encodestate* state_in, const char *encoding);

char base64_encode_value(char value_in, base64_encodestate* state_in);

int base64_encode_block(const char* plaintext_in, int length_in, char* code_out, base64_encodestate* state_in);

int base64_encode_blockend(char* code_out, base64_encodestate* state_in);

#endif /* BASE64_CENCODE_H */

