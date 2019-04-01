#ifndef __NETPLAY_STREAM_H__
#define __NETPLAY_STREAM_H__

class netplay_stream_writer
{
public:
	/*template <typename T>
	void write(const std::string& value)
	{
		write(value.length());
		write((void*)value.c_str(), (size_t)value.length());
	}

	template <typename T>
	void write(const T& value)
	{
		write((void*)&value, (size_t)sizeof(T));
	}

	template <typename T>
	void write(void* data_ptr, size_t size)
	{
		auto current_size = m_data.size();
		m_data.resize(current_size + size);

		auto dest = m_data.data() + current_size;
		memcpy(dest, data_ptr, size);
	}*/

	const std::vector<char>& get_data() const { return m_data; }

private:
	std::vector<char> m_data;
};

#endif
