#ifndef MAME_EMU_NETPLAY_UTIL_H
#define MAME_EMU_NETPLAY_UTIL_H

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

	void offset_cursor(int offset) { m_cursor = (m_cursor + offset) % std::min(m_buffer.size(), N); }
	bool empty() const { return m_buffer.empty(); }
	size_t size() const { return m_buffer.size(); }
	size_t capacity() const { return N; }
	T& newest() { return m_cursor == 0 ? m_buffer.back() : m_buffer[m_cursor - 1]; }
	const std::vector<T>& items() const { return m_buffer; }

	auto begin() { return m_buffer.begin(); }
	auto begin() const { return m_buffer.begin(); }
	auto end() { return m_buffer.end(); }
	auto end() const { return m_buffer.end(); }

private:
	std::vector<T> m_buffer;
	size_t m_cursor;
};

#endif
