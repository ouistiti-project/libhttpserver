/*
cencoder.c - c source to a base64 encoding algorithm implementation

This is part of the libb64 project, and has been placed in the public domain.
For details, see http://sourceforge.net/projects/libb64
*/
#include <b64/cencode.h>

int LIBB64_URLENCODING = 0;
static const char encoding_std[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/\0";
static const char encoding_url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_=";

void base64_init_encodestate(base64_encodestate* state_in)
{
	if (LIBB64_URLENCODING)
	{
		state_in->encoding = encoding_url;
		state_in->trailing_char = encoding_url[64];
	}
	else
	{
		state_in->encoding = encoding_std;
		state_in->trailing_char = encoding_url[64];
	}
	state_in->step = step_A;
	state_in->result = 0;
	state_in->stepcount = 0;
}

char base64_encode_value(char value_in, base64_encodestate* state_in)
{
	if (value_in > 63) return state_in->trailing_char;
	return state_in->encoding[(int)value_in];
}

int base64_encode_block(const char* plaintext_in, int length_in, char* code_out, base64_encodestate* state_in)
{
	const char* plainchar = plaintext_in;
	const char* const plaintextend = plaintext_in + length_in;
	char* codechar = code_out;
	char result;
	char fragment;

	result = state_in->result;

	switch (state_in->step)
	{
		while (1)
		{
	case step_A:
			if (plainchar == plaintextend)
			{
				state_in->result = result;
				state_in->step = step_A;
				return codechar - code_out;
			}
			fragment = *plainchar++;
			result = (fragment & 0x0fc) >> 2;
			*codechar++ = base64_encode_value(result, state_in);
			result = (fragment & 0x003) << 4;
	case step_B:
			if (plainchar == plaintextend)
			{
				state_in->result = result;
				state_in->step = step_B;
				return codechar - code_out;
			}
			fragment = *plainchar++;
			result |= (fragment & 0x0f0) >> 4;
			*codechar++ = base64_encode_value(result, state_in);
			result = (fragment & 0x00f) << 2;
	case step_C:
			if (plainchar == plaintextend)
			{
				state_in->result = result;
				state_in->step = step_C;
				return codechar - code_out;
			}
			fragment = *plainchar++;
			result |= (fragment & 0x0c0) >> 6;
			*codechar++ = base64_encode_value(result, state_in);
			result  = (fragment & 0x03f) >> 0;
			*codechar++ = base64_encode_value(result, state_in);

			++(state_in->stepcount);
		}
	}
	/* control should not reach here */
	return codechar - code_out;
}

int base64_encode_blockend(char* code_out, base64_encodestate* state_in)
{
	char* codechar = code_out;

	switch (state_in->step)
	{
	case step_B:
		*codechar++ = base64_encode_value(state_in->result, state_in);
		*codechar++ = state_in->trailing_char;
		*codechar++ = state_in->trailing_char;
		break;
	case step_C:
		*codechar++ = base64_encode_value(state_in->result, state_in);
		*codechar++ = state_in->trailing_char;
		break;
	case step_A:
		break;
	}
	*codechar = '\0';

	return codechar - code_out;
}

