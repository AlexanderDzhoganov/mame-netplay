#include <string.h>
#include <string>
#include <vector>

#include "lzma/C/LzmaDec.h"
#include "lzma/C/LzmaEnc.h"

#include "netplay.h"
#include "netplay/serialization.h"

#define LZMA_COMPRESSION_LEVEL 9

template <typename T>
void netplay_stream_writer<T>::write(unsigned int value)
{
	unsigned char mask = 0;
	for (auto i = 0u; i < 4; i++)
		if ((value >> (i << 3)) & 0xFF)
			mask |= (1 << i);

	write(mask);

	for (auto i = 0u; i < 4; i++)
		if ((value >> (i << 3)) & 0xFF)
			write((unsigned char)((value >> (i << 3)) & 0xFF));
}

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
void netplay_stream_reader<T>::read(unsigned int& value)
{
	value = 0;

	unsigned char mask;
	read(mask);

	if (mask == 0)
		return;

	unsigned char byte;
	for (auto i = 0u; i < 4; i++)
	{
		if (mask & (1 << i))
		{
			read(byte);
			value |= ((unsigned int)byte << (i << 3));
		}
	}
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

void* netplay_lzma_alloc(void*, size_t size)
{
	return new char[size];
}

void netplay_lzma_free(void*, void* ptr)
{
	delete [] (char*)ptr;
}

ISzAlloc netplay_lzma_alloc_fn = {&netplay_lzma_alloc, &netplay_lzma_free};

// this props array depends on the CLzmaEncProps passed to LzmaEncode
// if those are changed then the values here must be changed too
unsigned char netplay_lzma_props[] = {0x5d, 0x0, 0x0, 0x1, 0x0};

size_t netplay_max_compressed_size(size_t size) { return size + size / 3 + 128; }

bool netplay_compress(const char* src, size_t src_size, const char* dst, size_t& dst_size)
{
	netplay_assert(src != nullptr);
	netplay_assert(src_size > 0);
	netplay_assert(dst != nullptr);

	static CLzmaEncProps props;
	static bool props_initialized = false;

	if (!props_initialized)
	{
		LzmaEncProps_Init(&props);
		props.level = LZMA_COMPRESSION_LEVEL;
		props.dictSize = 65536;
		props.writeEndMark = 0;
		LzmaEncProps_Normalize(&props);
		props_initialized = true;
	}

	size_t props_size = LZMA_PROPS_SIZE;

	auto result = LzmaEncode(
		(unsigned char*)dst, &dst_size,
		(unsigned char*)src, src_size,
		&props, netplay_lzma_props, &props_size,
		0, 0, &netplay_lzma_alloc_fn, &netplay_lzma_alloc_fn
	);

	return result == SZ_OK;
}

bool netplay_decompress(const char* src, size_t src_size, const char* dst, size_t dst_size)
{
	netplay_assert(src != nullptr);
	netplay_assert(src_size > 0);
	netplay_assert(dst != nullptr);
	netplay_assert(dst_size > 0);

	auto result = LzmaDecode(
		(unsigned char*)dst, &dst_size,
		(unsigned char*)src, &src_size,
		netplay_lzma_props, LZMA_PROPS_SIZE,
		LZMA_FINISH_END, 0, &netplay_lzma_alloc_fn
	);

	return result == SZ_OK;
}
