#include <string.h>
#include <string>
#include <vector>

#include "netplay.h"
#include "netplay/serialization.h"

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
