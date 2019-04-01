#ifndef __NETPLAY_MEMORY_BLOCK_H__
#define __NETPLAY_MEMORY_BLOCK_H__

class netplay_memory_block
{
public:
	netplay_memory_block(unsigned int index, const std::string& name, size_t size);
	netplay_memory_block(unsigned int index, const std::string& name, void* data_ptr, size_t size);
	~netplay_memory_block();

	void copy_from(const netplay_memory_block& block);
	unsigned char checksum() const;

	unsigned int get_index() const { return m_index; }
	const std::string& get_name() const { return m_name; }
	void* get_data_ptr() const { return m_data_ptr; }
	size_t get_size() const { return m_size; }
	bool owns_memory() const { return m_owns_memory; }
	int get_generation() const { return m_generation; }
	void set_generation(int generation) { m_generation = generation; }
	bool dirty() const { return m_dirty; }
	void set_dirty(bool dirty) { m_dirty = dirty; }
	std::string get_debug_string();

private:
	unsigned int m_index;
	std::string m_name;
	char* m_data_ptr;
	size_t m_size;
	bool m_owns_memory;
	int m_generation;
	bool m_dirty;
};

#endif
