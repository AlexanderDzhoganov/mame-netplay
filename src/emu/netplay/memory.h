#ifndef MAME_EMU_NETPLAY_MEMORY_H
#define MAME_EMU_NETPLAY_MEMORY_H

class netplay_memory
{
	DISABLE_COPYING(netplay_memory);

public:
	netplay_memory(unsigned int index, const std::string& module_name, const std::string& name, size_t size);
	netplay_memory(unsigned int index, const std::string& module_name, const std::string& name, void* data, size_t size);
	~netplay_memory();

	void copy_from(const netplay_memory& block);

	unsigned int index() const { return m_index; }
	unsigned int module_hash() const { return m_module_hash; }
	const std::string& name() const { return m_name; }
	const std::string& module_name() const { return m_module_name; }
	void* data() const { return m_data; }
	size_t size() const { return m_size; }
	bool owns_memory() const { return m_owns_memory; }
	unsigned int checksum();
	std::string debug_string();

private:
	size_t m_size;
	unsigned int m_index;
	unsigned int m_module_hash;
	std::string m_module_name;
	std::string m_name;
	char* m_data;
	bool m_owns_memory;
	unsigned int m_checksum;
};

#endif
