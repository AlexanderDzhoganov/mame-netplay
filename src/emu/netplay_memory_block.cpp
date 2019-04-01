#include "emu.h"
#include "netplay_memory_block.h"

//-------------------------------------------------
// netplay_memory_block
//-------------------------------------------------

netplay_memory_block::netplay_memory_block
(
	unsigned int index,
	const std::string& name,
	size_t size
) :
	m_index(index), m_name(name), m_data_ptr(nullptr),
	m_size(size), m_owns_memory(true), m_generation(0), m_dirty(false)
{
	assert(size > 0);
	m_data_ptr = new char[size];
}

netplay_memory_block::netplay_memory_block
(
	unsigned int index,
	const std::string& name,
	void* data_ptr,
	size_t size
) :
	m_index(index), m_name(name), m_data_ptr((char*)data_ptr),
	m_size(size), m_owns_memory(false), m_generation(0), m_dirty(false)
{
	assert(data_ptr != nullptr);
	assert(size > 0);
}

netplay_memory_block::~netplay_memory_block()
{
	assert(m_data_ptr != nullptr);

	if (m_owns_memory)
	{
		delete [] m_data_ptr;
	}
}

void netplay_memory_block::copy_from(const netplay_memory_block& block)
{
	assert(m_size == block.m_size);
	memcpy(m_data_ptr, block.m_data_ptr, m_size);
	m_generation = block.m_generation;
}

unsigned char netplay_memory_block::checksum() const
{
	unsigned char checksum = 0;

	for (auto i = 0u; i < m_size; i++)
	{
		checksum ^= m_data_ptr[i];
	}

	return checksum;
}

std::string netplay_memory_block::get_debug_string()
{
	std::stringstream ss;
	ss << "memory block #" << m_index << " \"" << m_name << "\" @ " << (size_t)m_data_ptr;
	ss << " [ " << "size = " << m_size << ", gen  = " << m_generation;
	ss << ", owns_memory = " << (m_owns_memory ? "yes" : "no") << " ]";
	return ss.str();
}
