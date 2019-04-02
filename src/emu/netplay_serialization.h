#ifndef MAME_EMU_NETPLAY_SERIALIZATION_H
#define MAME_EMU_NETPLAY_SERIALIZATION_H

template <typename Stream>
class netplay_stream_writer
{
public:
	netplay_stream_writer(Stream& stream) : m_stream(stream) {}

	void header(char a, char b, char c, char d) { write(a); write(b); write(c); write(d); }
	void write(const std::string& value);
	void write(const attotime& value) { write(value.m_seconds); write(value.m_attoseconds); }
	void write(void* data, size_t size) { m_stream.write(data, size); }
	template <typename T> void write(const T& value) { write((void*)&value, (size_t)sizeof(T)); }

	Stream& stream() const { return m_stream; }

private:
	Stream& m_stream;
};

template <typename Stream>
class netplay_stream_reader
{
public:
	netplay_stream_reader(Stream& stream) : m_stream(stream) {}

	void header(char a, char b, char c, char d);
	void read(std::string& value);
	void read(attotime& value) { read(value.m_seconds); read(value.m_attoseconds); }
	void read(void* data, size_t size) { m_stream.read(data, size); }
	template <typename T> void read(T& value) { read((void*)&value, (size_t)sizeof(T)); }

	bool eof() const { return m_stream.eof(); }
	Stream& stream() const { return m_stream; }

private:
	Stream& m_stream;
};

class netplay_memory_stream
{
public:
	netplay_memory_stream() : m_cursor(0) {}
	netplay_memory_stream(std::vector<char> data) : m_data(std::move(data)), m_cursor(0) {}

	void write(void* data, size_t size);
	void read(void* data, size_t size);

	bool eof() const { return m_cursor >= m_data.size(); }
	size_t size() const { return m_data.size(); }

	void set_data(std::vector<char> data) { m_data = std::move(data); m_cursor = 0; }
	const std::vector<char>& data() { return m_data; }
	std::vector<char> extract_data() { return std::move(m_data); }

private:
	std::vector<char> m_data;
	size_t m_cursor;
};

typedef netplay_stream_writer<netplay_memory_stream> netplay_memory_writer;
typedef netplay_stream_reader<netplay_memory_stream> netplay_memory_reader;

#endif
