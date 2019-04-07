#ifndef MAME_EMU_NETPLAY_UTIL_H
#define MAME_EMU_NETPLAY_UTIL_H

#include <stdio.h>

#define NETPLAY_LOG(...) { printf(__VA_ARGS__); printf("\n"); }

#ifndef NO_NETPLAY_ASSERT
#define netplay_assert(COND) do { \
	if (!(COND)) { \
		printf("\n\nassertion failed: " #COND " (%s:%d)\n\n", __FILE__, __LINE__); \
		exit(1); \
	} \
} while(0);
#endif

typedef unsigned int netplay_frame;

template <typename T, size_t N>
class netplay_circular_buffer
{
public:
	netplay_circular_buffer() : m_cursor(0), m_size(0) {}

	void push_back(const T& value)
	{
		if (m_size > 0)
			advance(1);

		m_buffer[m_cursor] = value;
		if (m_size < N)
			m_size++;
	}

	void advance(int offset) { m_cursor = (m_cursor + offset) % N; }
	bool empty() const { return m_size == 0; }
	size_t size() const { return m_size; }
	size_t capacity() const { return N; }

	void clear()
	{
		m_cursor = 0;
		m_size = 0;
	}

	T& newest()
	{
		netplay_assert(m_cursor < m_size);
		return m_buffer[m_cursor];
	}

	const T& newest() const
	{
		netplay_assert(m_cursor < m_size);
		return m_buffer[m_cursor];
	}

	T& operator[] (size_t index)
	{
		netplay_assert(index < m_size);
		return m_buffer[index];
	}

	const T& operator[] (size_t index) const
	{
		netplay_assert(index < m_size);
		return m_buffer[index];
	}

private:
	std::array<T, N> m_buffer;
	size_t m_cursor;
	size_t m_size;
};

#endif
