/*
cencoder.c - c source to a base64 encoding algorithm implementation

This is part of the libb64 project, and has been placed in the public domain.
For details, see http://sourceforge.net/projects/libb64
*/
#include <b64/cencode.h>

const char base32_encoding_std[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567=";

void base32_init_encodestate(base64_encodestate* state_in, const char *encoding)
{
	state_in->encoding = encoding;
	state_in->trailing_char = encoding[32];
	state_in->step = step_A;
	state_in->result = 0;
	state_in->stepcount = 0;
}

char base32_encode_value(char value_in, base64_encodestate* state_in)
{
	if (value_in > 31) return state_in->trailing_char;
	return state_in->encoding[(int)value_in];
}

int base32_encode_block(const char* plaintext_in, int length_in, char* code_out, base64_encodestate* state_in)
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
			result = (fragment & 0x0f8) >> 3;
			*codechar++ = base32_encode_value(result, state_in);
			result = (fragment & 0x007) << 2;
	case step_B:
			if (plainchar == plaintextend)
			{
				state_in->result = result;
				state_in->step = step_B;
				return codechar - code_out;
			}
			fragment = *plainchar++;
			result |= (fragment & 0x0c0) >> 6;
			*codechar++ = base32_encode_value(result, state_in);
			result = (fragment & 0x03e) >> 1;
			*codechar++ = base32_encode_value(result, state_in);
			result = (fragment & 0x001) << 4;
	case step_C:
			if (plainchar == plaintextend)
			{
				state_in->result = result;
				state_in->step = step_C;
				return codechar - code_out;
			}
			fragment = *plainchar++;
			result |= (fragment & 0x0f0) >> 4;
			*codechar++ = base32_encode_value(result, state_in);
			result  = (fragment & 0x00f) << 1;
	case step_D:
			if (plainchar == plaintextend)
			{
				state_in->result = result;
				state_in->step = step_D;
				return codechar - code_out;
			}
			fragment = *plainchar++;
			result |= (fragment & 0x080) >> 7;
			*codechar++ = base32_encode_value(result, state_in);
			result  = (fragment & 0x07c) >> 2;
			*codechar++ = base32_encode_value(result, state_in);
			result  = (fragment & 0x003) << 3;
	case step_E:
			if (plainchar == plaintextend)
			{
				state_in->result = result;
				state_in->step = step_E;
				return codechar - code_out;
			}
			fragment = *plainchar++;
			result |= (fragment & 0x0e0) >> 5;
			*codechar++ = base32_encode_value(result, state_in);
			result  = (fragment & 0x01f) >> 0;
			*codechar++ = base32_encode_value(result, state_in);

			++(state_in->stepcount);
		}
	}
	/* control should not reach here */
	return codechar - code_out;
}

int base32_encode_blockend(char* code_out, base64_encodestate* state_in)
{
	char* codechar = code_out;

	switch (state_in->step)
	{
	case step_B:
		*codechar++ = base32_encode_value(state_in->result, state_in);
		*codechar++ = state_in->trailing_char;
		*codechar++ = state_in->trailing_char;
		*codechar++ = state_in->trailing_char;
		*codechar++ = state_in->trailing_char;
		*codechar++ = state_in->trailing_char;
		*codechar++ = state_in->trailing_char;
		break;
	case step_C:
		*codechar++ = base32_encode_value(state_in->result, state_in);
		*codechar++ = state_in->trailing_char;
		*codechar++ = state_in->trailing_char;
		*codechar++ = state_in->trailing_char;
		*codechar++ = state_in->trailing_char;
		break;
	case step_D:
		*codechar++ = base32_encode_value(state_in->result, state_in);
		*codechar++ = state_in->trailing_char;
		*codechar++ = state_in->trailing_char;
		*codechar++ = state_in->trailing_char;
		break;
	case step_E:
		*codechar++ = base32_encode_value(state_in->result, state_in);
		*codechar++ = state_in->trailing_char;
		break;
	case step_A:
		break;
	}
	*codechar = '\0';

	return codechar - code_out;
}

