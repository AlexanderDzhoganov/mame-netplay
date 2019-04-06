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

template <typename T, size_t N>
class netplay_circular_buffer
{
public:
	netplay_circular_buffer() : m_cursor(0), m_capacity(N)
	{
		m_buffer.reserve(N);
	}

	void push_back(const T& value)
	{
		if (m_buffer.size() < m_capacity)
			m_buffer.push_back(value);
		else
			m_buffer[m_cursor] = value;

		if(m_buffer.size() > 1)
			m_cursor = (m_cursor + 1) % m_buffer.size();
	}

	bool empty() const { return m_buffer.empty(); }
	size_t size() const { return m_buffer.size(); }
	size_t capacity() const { return m_capacity; }
	const std::vector<T>& items() const { return m_buffer; }

	void set_capacity(size_t capacity)
	{
		m_capacity = capacity; 
		m_buffer.reserve(capacity);
		m_buffer.clear();
	}

	void clear()
	{
		m_buffer.clear();
		m_cursor = 0;
	}

	T& newest()
	{
		netplay_assert(m_cursor < m_buffer.size());
		return m_buffer[m_cursor];
	}

	const T& newest() const
	{
		netplay_assert(m_cursor < m_buffer.size());
		return m_buffer[m_cursor];
	}
	
	void advance(int offset)
	{ 
		m_cursor = (m_cursor + offset) % m_buffer.size();
	}

	auto begin() { return m_buffer.begin(); }
	auto begin() const { return m_buffer.begin(); }
	auto end() { return m_buffer.end(); }
	auto end() const { return m_buffer.end(); }

private:
	std::vector<T> m_buffer;
	size_t m_cursor;
	size_t m_capacity;
};

#endif
