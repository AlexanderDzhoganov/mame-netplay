#include <string.h>
#include <string>
#include <vector>

#include "lzma/C/LzmaDec.h"
#include "lzma/C/LzmaEnc.h"

#include "netplay.h"
#include "netplay/serialization.h"

#define LZMA_COMPRESSION_LEVEL 9

template <typename T>
void netplay_stream_writer<T>::write(const std::string& value)
{
	write((unsigned int)value.length());
	write((void*)value.c_str(), (size_t)value.length());
}

template <typename T>
void netplay_stream_reader<T>::header(char a, char b, char c, char d)
{
#ifdef NETPLAY_DEBUG
	char arr[4];
	read(arr, 4);

	if (arr[0] != a || arr[1] != b || arr[2] != c || arr[3] != d)
	{
		NETPLAY_LOG("(WARNING) ENCOUNTERED AN INVALID HEADER DURING DESERIALIZATION.");
		NETPLAY_LOG("(WARNING) EXPECTED = '%c%c%c%c' INSTEAD FOUND = '%c%c%c%c'", a, b, c, d, arr[0], arr[1], arr[2], arr[3]);
		NETPLAY_LOG("(WARNING) THIS IS MOST LIKELY A BUG.");

		// maybe we should throw here?
	}
#endif
}

template <typename T>
void netplay_stream_reader<T>::read(std::string& value)
{
	unsigned int length;
	read(length);
	value.resize(length);

	read((void*)value.c_str(), length);
}

void netplay_memory_stream::write(void* data, size_t size)
{
	netplay_assert(data != nullptr);
	netplay_assert(size > 0);

	if (m_cursor + size >= m_data.size())
	{
		m_data.resize(m_cursor + size);
	}

	memcpy(m_data.data() + m_cursor, data, size);
	m_cursor += size;
}

void netplay_memory_stream::read(void* data, size_t size)
{
	netplay_assert(data != nullptr);
	netplay_assert(size > 0);
	netplay_assert(m_cursor + size <= m_data.size());

	memcpy(data, m_data.data() + m_cursor, size);
	m_cursor += size;
}

void netplay_raw_byte_stream::read(void* data, size_t size)
{
	netplay_assert(m_cursor + size <= m_size);

	memcpy(data, m_data + m_cursor, size);
	m_cursor += size;
}

template class netplay_stream_writer<netplay_memory_stream>;
template class netplay_stream_reader<netplay_raw_byte_stream>;

void* netplay_lzma_alloc(void* ptr, size_t size) { return (void*)new char[size]; }
void netplay_lzma_free(void*, void* ptr) { delete[] (char*)ptr; }
ISzAlloc netplay_lzma_alloc_fn = {&netplay_lzma_alloc, &netplay_lzma_free};

size_t netplay_max_compressed_size(size_t size) { return size + size / 3 + 128 + LZMA_PROPS_SIZE; }

bool netplay_compress(const char* src, size_t src_size, const char* dst, size_t& dst_size)
{
	netplay_assert(src != nullptr);
	netplay_assert(src_size > 0);
	netplay_assert(dst != nullptr);

	CLzmaEncProps props;
  LzmaEncProps_Init(&props);
	props.level = LZMA_COMPRESSION_LEVEL;
	props.dictSize = 65536;
	props.writeEndMark = 0;
	LzmaEncProps_Normalize(&props);

	size_t props_size = LZMA_PROPS_SIZE;

	if(LzmaEncode(
		(unsigned char*)(dst + LZMA_PROPS_SIZE), &dst_size,
		(unsigned char*)src, src_size,
		&props, (unsigned char*)dst, &props_size,
		0, 0, &netplay_lzma_alloc_fn, &netplay_lzma_alloc_fn
	) != SZ_OK)
		return false;

	dst_size += LZMA_PROPS_SIZE;
	return true;
}

bool netplay_decompress(const char* src, size_t src_size, const char* dst, size_t dst_size)
{
	netplay_assert(src != nullptr);
	netplay_assert(src_size > 0);
	netplay_assert(dst != nullptr);
	netplay_assert(dst_size > 0);

	src_size -= LZMA_PROPS_SIZE;
	return LzmaDecode(
		(unsigned char*)dst, &dst_size,
		(unsigned char*)(src + LZMA_PROPS_SIZE), &src_size,
		(unsigned char*)src, LZMA_PROPS_SIZE,
		LZMA_FINISH_END, 0, &netplay_lzma_alloc_fn
	) == SZ_OK;
}
