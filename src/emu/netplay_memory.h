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
	void copy_from(void* data, size_t size, int generation);
	unsigned char checksum() const;

	unsigned int index() const { return m_index; }
	const std::string& get_name() const { return m_name; }
	void* data() const { return m_data; }
	size_t size() const { return m_size; }
	bool owns_memory() const { return m_owns_memory; }
	int generation() const { return m_generation; }
	void set_generation(int generation) { m_generation = generation; }
	std::string get_debug_string();

private:
	size_t m_size;
	unsigned int m_index;
	std::string m_name;
	char* m_data;
	bool m_owns_memory;
	int m_generation;
};

#endif
