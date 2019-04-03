#ifndef MAME_EMU_NETPLAY_UTIL_H
#define MAME_EMU_NETPLAY_UTIL_H

template <typename T, int N>
class netplay_circular_buffer
{
public:
	netplay_circular_buffer() : m_cursor(0)
	{
		m_buffer.reserve(N);
	}

	void insert(const T& value)
	{
		if (m_cursor < N)
		{
			m_buffer.push_back(value);
		}
		else
		{
			m_buffer[m_cursor] = value;
		}

		m_cursor++;
		if (m_cursor == N)
		{
			m_cursor = 0;
		}
	}

	void insert(T&& value)
	{
		m_buffer.push_back(value);
	}

	const std::vector<T>& items() const { return m_buffer; }
	size_t size() const { return m_buffer.size(); }

private:
	std::vector<T> m_buffer;
	size_t m_cursor;
};

#endif
