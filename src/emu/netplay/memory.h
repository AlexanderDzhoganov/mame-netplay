#ifndef MAME_EMU_NETPLAY_MEMORY_H
#define MAME_EMU_NETPLAY_MEMORY_H

class netplay_memory
{
	DISABLE_COPYING(netplay_memory);

public:
	netplay_memory(unsigned int index, const std::string& name, size_t size);
	netplay_memory(unsigned int index, const std::string& name, void* data, size_t size);
	~netplay_memory();

	void copy_from(const netplay_memory& block);
	void copy_from(void* data, size_t size);

	unsigned int index() const { return m_index; }
	const std::string& name() const { return m_name; }
	void* data() const { return m_data; }
	size_t size() const { return m_size; }
	bool owns_memory() const { return m_owns_memory; }
	std::string get_debug_string();
	
	unsigned char checksum() const;
	static unsigned char checksum(const netplay_blocklist& blocks);

private:
	size_t m_size;
	unsigned int m_index;
	std::string m_name;
	char* m_data;
	bool m_owns_memory;
};

#endif
