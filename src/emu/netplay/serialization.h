#ifndef MAME_EMU_NETPLAY_SERIALIZATION_H
#define MAME_EMU_NETPLAY_SERIALIZATION_H

// #define NETPLAY_DEBUG

template <typename Stream>
class netplay_stream_writer
{
public:
	netplay_stream_writer() {}

#ifdef NETPLAY_DEBUG
	void header(char a, char b, char c, char d) { write(a); write(b); write(c); write(d); }
#else
	void header(char a, char b, char c, char d) {} // this is a no-op when not compiled in debug mode
#endif

	void write(bool value) { write(value ? (char)1 : (char)0); }
	void write(const std::string& value);
	void write(const attotime& value) { write(value.as_double()); }
	void write(void* data, size_t size) { m_stream.write(data, size); }
	template <typename T> void write(const T& value) { write((void*)&value, (size_t)sizeof(T)); }

	Stream& stream() { return m_stream; }

private:
	Stream m_stream;
};

template <typename Stream>
class netplay_stream_reader
{
public:
	netplay_stream_reader(Stream& stream) : m_stream(stream) {}

	void header(char a, char b, char c, char d);
	void read(bool& value) { char c; read(c); value = c == (char)1; }
	void read(std::string& value);
	void read(attotime& value) { double d; read(d); value = attotime::from_double(d); }
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
	size_t cursor() const { return m_cursor; }

	const std::vector<char>& data() { return m_data; }

private:
	std::vector<char> m_data;
	size_t m_cursor;
};

class netplay_raw_byte_stream
{
public:
	netplay_raw_byte_stream(const char* data, size_t size) :
		m_data(data), m_size(size), m_cursor(0) {}

	void read(void* data, size_t size);

	bool eof() const { return m_cursor >= m_size; }
	size_t size() const { return m_size; }

private:
	const char* m_data;
	size_t m_size;
	size_t m_cursor;
};

typedef netplay_stream_writer<netplay_memory_stream> netplay_memory_writer;
typedef netplay_stream_reader<netplay_memory_stream> netplay_memory_reader;

size_t netplay_max_compressed_size(size_t size);
bool netplay_compress(const char* src, size_t src_size, const char* dst, size_t& dst_size);
bool netplay_decompress(const char* src, size_t src_size, const char* dst, size_t dst_size);

#endif
