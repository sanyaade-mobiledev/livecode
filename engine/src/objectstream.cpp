/* Copyright (C) 2003-2013 Runtime Revolution Ltd.

This file is part of LiveCode.

LiveCode is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License v3 as published by the Free
Software Foundation.

LiveCode is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with LiveCode.  If not see <http://www.gnu.org/licenses/>.  */

#include "prefix.h"

#include "core.h"
#include "globdefs.h"
#include "filedefs.h"
#include "objdefs.h"
#include "parsedef.h"
#include "mcio.h"
#include "globals.h"

#include "object.h"
#include "objectstream.h"

MCObjectInputStream::MCObjectInputStream(IO_handle p_stream, uint32_t p_remaining)
{
	m_stream = p_stream;
	m_buffer = NULL;
	m_frontier = 0;
	m_limit = 0;
	m_bound = 0;
	m_remaining = p_remaining;
	m_mark = 0;
}

MCObjectInputStream::~MCObjectInputStream(void)
{
	delete (char *)m_buffer;
}

// Flushing reads and discards the rest of the stream
IO_stat MCObjectInputStream::Flush(void)
{
	return Read(NULL, m_remaining);
}

// The mark point is 0 where the current frontier is. It is updated whenever we will thus meaning that:
//   <stream pos> + m_mark - m_frontier
// Is the location in the stream we marked.
IO_stat MCObjectInputStream::Mark(void)
{
	m_mark = m_frontier;
	return IO_NORMAL;
}

IO_stat MCObjectInputStream::Skip(uint32_t p_length)
{
	// Take into account the current frontier
	m_mark -= m_frontier;

	// Calculate where we want the frontier to be
	m_mark += p_length;

	// We should never be skipping backwards. If we are its an error in the stream
	if (m_mark < 0)
		return IO_ERROR;

	IO_stat t_stat;

	// If mark is positive we have to advance the frontier by that amount
	if (m_mark > 0)
		t_stat = Read(NULL, m_mark);
	else
		t_stat = IO_NORMAL;

	// At this point we should have that the frontier is pointing to the original mark
	// offset by length.

	return t_stat;
}

IO_stat MCObjectInputStream::ReadTag(uint32_t& r_flags, uint32_t& r_length, uint32_t& r_header_length)
{
	IO_stat t_stat;

	uint32_t t_tag;
	t_stat = ReadU32(t_tag);
	if (t_stat == IO_NORMAL && (t_tag & (1U << 31)) == 0)
	{
		// Top bit not set means its 23:8 tag.
		// Top 23 bits are length, bottom 8 bits are flags.
		r_flags = t_tag & 0xFF;
		r_length = t_tag >> 8;
		r_header_length = 4;
	}
	else if (t_stat == IO_NORMAL)
	{
		uint32_t t_extension;
		if (t_stat == IO_NORMAL)
			t_stat = ReadU32(t_extension);
		if (t_stat == IO_NORMAL)
		{
			r_flags = (t_tag & 0xFF) | ((t_extension & 0xFFFFFF) << 8);
			
			// MW-2010-05-06: Mask for the upper 24 bits of t_tag was incorrect, it should
			//   contain all but the 24th bit. (Reported in [[Bug 8716]]).
			r_length = ((t_tag >> 8) & 0x7fffff) | ((t_extension & 0xFF000000) >> 1);
			r_header_length = 8;
		}
	}

	return t_stat;
}

IO_stat MCObjectInputStream::ReadFloat32(float& r_value)
{
	uint32_t t_bits;

	IO_stat t_stat;
	t_stat = ReadU32(t_bits);
	if (t_stat == IO_NORMAL)
		memcpy(&r_value, &t_bits, sizeof(float));

	return t_stat;
}

// MW-2011-09-12: Hit an ICE in GCC for ARM when using &r_value directly.
IO_stat MCObjectInputStream::ReadFloat64(double& r_value)
{
	uint64_t t_bits;
	double t_value;

	IO_stat t_stat;
	t_stat = ReadU64(t_bits);
	if (t_stat == IO_NORMAL)
	{
		memcpy(&t_value, &t_bits, sizeof(double));
		r_value = t_value;
	}
		
	return t_stat;
}

IO_stat MCObjectInputStream::ReadU8(uint8_t& r_value)
{
	return Read(&r_value, 1);
}

IO_stat MCObjectInputStream::ReadU16(uint16_t& r_value)
{
	IO_stat t_stat;
	t_stat = Read(&r_value, 2);
	if (t_stat == IO_NORMAL)
		r_value = MCSwapInt16NetworkToHost(r_value);
	return t_stat;
}

IO_stat MCObjectInputStream::ReadU32(uint32_t& r_value)
{
	IO_stat t_stat;
	t_stat = Read(&r_value, 4);
	if (t_stat == IO_NORMAL)
		r_value = MCSwapInt32NetworkToHost(r_value);

	return t_stat;
}

IO_stat MCObjectInputStream::ReadU64(uint64_t& r_value)
{
	IO_stat t_stat;
	t_stat = Read(&r_value, 8);
	if (t_stat == IO_NORMAL)
#ifndef __LITTLE_ENDIAN__
		r_value = r_value;
#else
		r_value = ((r_value >> 56) | ((r_value >> 40) & 0xFF00) | ((r_value >> 24) & 0xFF0000) | ((r_value >> 8) & 0xFF000000) |
				  ((r_value & 0xFF000000) << 8) | ((r_value & 0xFF0000) << 24) | ((r_value & 0xFF00) << 40) | (r_value << 56));
#endif
	return t_stat;
}

IO_stat MCObjectInputStream::ReadS16(int16_t& r_value)
{
	IO_stat t_stat;
	t_stat = Read(&r_value, 2);
	if (t_stat == IO_NORMAL)
		r_value = (signed short)MCSwapInt16NetworkToHost((unsigned short)r_value);
	return t_stat;
}

//

IO_stat MCObjectInputStream::ReadCString(char*& r_value)
{
	uint32_t t_length;
	t_length = 0;

	char *t_output;
	t_output = NULL;

	bool t_finished;
	t_finished = false;

	while(!t_finished)
	{
		if (m_limit == m_frontier)
		{
			IO_stat t_stat;
			t_stat = Fill();
			if (t_stat != IO_NORMAL)
				return t_stat;
		}

		uint32_t t_offset;
		for(t_offset = 0; t_offset < m_limit - m_frontier; ++t_offset)
			if (((char *)m_buffer)[m_frontier + t_offset] == '\0')
			{
				t_offset += 1;
				t_finished = true;
				break;
			}

		uint32_t t_new_length;
		t_new_length = t_length + t_offset;

		char *t_new_output;
		t_new_output = (char *)realloc(t_output, t_new_length);
		if (t_new_output == NULL)
		{
			free(t_output);
			return IO_ERROR;
		}

		memcpy(t_new_output + t_length, (char *)m_buffer + m_frontier, t_offset);

		t_output = t_new_output;
		t_length = t_new_length;

		m_frontier += t_offset;
	}

	if (t_output != NULL)
	{
		if (t_output[0] == '\0')
		{
			r_value = NULL;
			free(t_output);
		}
		else
			r_value = t_output;
	}
	else
		r_value = NULL;

	return IO_NORMAL;
}

IO_stat MCObjectInputStream::ReadNameRef(MCNameRef& r_value)
{
	char *t_name_cstring;
	t_name_cstring = nil;

	IO_stat t_stat;
	t_stat = ReadCString(t_name_cstring);
	if (t_stat == IO_NORMAL &&
		!MCNameCreateWithCString(t_name_cstring != nil ? t_name_cstring : MCnullstring, r_value))
		t_stat = IO_ERROR;

	free(t_name_cstring);

	return t_stat;
}

IO_stat MCObjectInputStream::ReadColor(MCColor &r_color)
{
	IO_stat t_stat = IO_NORMAL;

	if (t_stat == IO_NORMAL)
		t_stat = ReadU16(r_color . red);
	if (t_stat == IO_NORMAL)
		t_stat = ReadU16(r_color . green);
	if (t_stat == IO_NORMAL)
		t_stat = ReadU16(r_color . blue);

	return t_stat;
}

//

IO_stat MCObjectInputStream::Read(void *p_buffer, uint32_t p_amount)
{
	while(p_amount > 0)
	{
		if (m_limit == m_frontier)
		{
			IO_stat t_stat;
			t_stat = Fill();
			if (t_stat != IO_NORMAL)
				return t_stat;
		}

		uint32_t t_available;
		t_available = MCU_min(m_limit - m_frontier, p_amount);

		if (p_buffer != NULL)
		{
			memcpy(p_buffer, (char *)m_buffer + m_frontier, t_available);
			p_buffer = (char *)p_buffer + t_available;
		}

		p_amount -= t_available;
		m_frontier += t_available;
	}

	return IO_NORMAL;
}

IO_stat MCObjectInputStream::Fill(void)
{
	if (m_remaining == 0)
		return IO_EOF;

	IO_stat t_stat;

	if (m_buffer == nil)
		m_buffer = new char[16384];
	
	if (m_buffer == nil)
		return IO_ERROR;
	
	// Move remaining data to start of buffer
	memmove(m_buffer, (char *)m_buffer + m_frontier, m_bound - m_frontier);
	m_limit -= m_frontier;
	m_bound -= m_frontier;
	m_mark -= m_frontier;
	m_frontier = 0;

	// Compute the amount of data to read - this is the minimum of the remaining
	// number of bytes and the remaining space in the buffer. The buffer is fixed
	// at 16K.
	uint32_t t_available;
	t_available = MCU_min(m_remaining, 16384 - m_bound);

	uint32_t t_count;
	t_count = 1;

	t_stat = IO_read((char *)m_buffer + m_bound, t_available, t_count, m_stream);
	if (t_stat != IO_NORMAL)
		return t_stat;

	m_bound += t_available;
	m_remaining -= t_available;
	m_limit += t_available;

	return t_stat;
}

///////////////////////////////////////////////////////////////////////////////

MCObjectOutputStream::MCObjectOutputStream(IO_handle p_stream)
{
	m_stream = p_stream;

	m_buffer = new char[16384];
	m_frontier = 0;
	m_mark = 0;
}

MCObjectOutputStream::~MCObjectOutputStream(void)
{
	delete (char *)m_buffer;
}

IO_stat MCObjectOutputStream::WriteTag(uint32_t p_flags, uint32_t p_length)
{
	if (p_flags <= 255 && p_length < 1 << 23)
		return WriteU32(p_flags | (p_length << 8));

	IO_stat t_stat;
	t_stat = WriteU32((p_flags & 0xFF) | ((p_length & 0x7FFFFF) << 8) | (1U << 31));
	if (t_stat == IO_NORMAL)
		t_stat = WriteU32((p_flags >> 8) | ((p_length >> 23) << 24));

	return t_stat;
}

IO_stat MCObjectOutputStream::WriteFloat32(float p_value)
{
	uint32_t t_value;
	memcpy(&t_value, &p_value, sizeof(float));
	return WriteU32(t_value);
}

IO_stat MCObjectOutputStream::WriteFloat64(double p_value)
{
	uint64_t t_value;
	memcpy(&t_value, &p_value, sizeof(double));
	return WriteU64(t_value);
}

IO_stat MCObjectOutputStream::WriteU8(uint8_t p_value)
{
	return Write(&p_value, 1);
}

IO_stat MCObjectOutputStream::WriteU16(uint16_t p_value)
{
	p_value = MCSwapInt16HostToNetwork(p_value);
	return Write(&p_value, 2);
}

IO_stat MCObjectOutputStream::WriteU32(uint32_t p_value)
{
	p_value = MCSwapInt32HostToNetwork(p_value);
	return Write(&p_value, 4);
}

IO_stat MCObjectOutputStream::WriteU64(uint64_t p_value)
{
#ifndef __LITTLE_ENDIAN__
	p_value = p_value;
#else
	p_value = ((p_value >> 56) | ((p_value >> 40) & 0xFF00) | ((p_value >> 24) & 0xFF0000) | ((p_value >> 8) & 0xFF000000) |
			  ((p_value & 0xFF000000) << 8) | ((p_value & 0xFF0000) << 24) | ((p_value & 0xFF00) << 40) | (p_value << 56));
#endif
	return Write(&p_value, 8);
}

IO_stat MCObjectOutputStream::WriteCString(const char *p_value)
{
	if (p_value == NULL)
		return WriteU8(0);

	uint32_t t_length;
	t_length = strlen(p_value) + 1;
	return Write(p_value, t_length);
}

IO_stat MCObjectOutputStream::WriteNameRef(MCNameRef p_value)
{
	return WriteCString(MCNameGetCString(p_value));
}

IO_stat MCObjectOutputStream::WriteColor(const MCColor &p_value)
{
	IO_stat t_stat = IO_NORMAL;
	if (t_stat == IO_NORMAL)
		t_stat = WriteU16(p_value.red);
	if (t_stat == IO_NORMAL)
		t_stat = WriteU16(p_value.green);
	if (t_stat == IO_NORMAL)
		t_stat = WriteU16(p_value.blue);

	return t_stat;
}

IO_stat MCObjectOutputStream::Write(const void *p_buffer, uint32_t p_amount)
{
	while(p_amount > 0)
	{
		if (m_frontier == 16384)
		{
			IO_stat t_stat;
			t_stat = Flush(false);
			if (t_stat != IO_NORMAL)
				return t_stat;
		}

		uint32_t t_available;
		t_available = MCU_min(16384 - m_frontier, p_amount);

		memcpy((char *)m_buffer + m_frontier, p_buffer, t_available);

		p_buffer = (char *)p_buffer + t_available;
		p_amount -= t_available;
		m_frontier += t_available;
	}

	return IO_NORMAL;
}

IO_stat MCObjectOutputStream::Flush(bool p_end)
{
	m_mark = m_frontier;

	uint32_t t_count;
	t_count = 1;

	IO_stat t_stat;
	t_stat = MCS_write(m_buffer, m_mark, t_count, m_stream);
	if (t_stat != IO_NORMAL)
		return t_stat;

	memmove(m_buffer, (char *)m_buffer + m_mark, m_frontier - m_mark);
	m_frontier -= m_mark;
	m_mark = 0;

	return IO_NORMAL;
}
