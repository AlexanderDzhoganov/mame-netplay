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
	netplay_circular_buffer() : m_cursor(0)
	{
		m_buffer.reserve(N);
	}

	void push_back(const T& value)
	{
		if (m_cursor < N)
			m_buffer.push_back(value);
		else
			m_buffer[m_cursor] = value;

		m_cursor = (m_cursor + 1) % N;
	}

	bool empty() const { return m_buffer.empty(); }
	size_t size() const { return m_buffer.size(); }
	size_t capacity() const { return N; }
	const std::vector<T>& items() const { return m_buffer; }

	T& newest()
	{
		netplay_assert(!m_buffer.empty());
		return m_cursor == 0 ? m_buffer.back() : m_buffer[m_cursor - 1];
	}

	const T& newest() const
	{
		netplay_assert(!m_buffer.empty());
		return m_cursor == 0 ? m_buffer.back() : m_buffer[m_cursor - 1];
	}
	
	void advance(int offset)
	{ 
		m_cursor = (m_cursor + offset) % std::min(m_buffer.size(), N);
	}

	auto begin() { return m_buffer.begin(); }
	auto begin() const { return m_buffer.begin(); }
	auto end() { return m_buffer.end(); }
	auto end() const { return m_buffer.end(); }

private:
	std::vector<T> m_buffer;
	size_t m_cursor;
};

#endif
