#include "emu.h"
#include "netplay_memory.h"

//-------------------------------------------------
// netplay_memory
//-------------------------------------------------

netplay_memory::netplay_memory
(
	unsigned int index,
	const std::string& name,
	size_t size
) :
	m_size(size), m_index(index), m_name(name), m_data(nullptr),
	m_owns_memory(true), m_generation(0), m_dirty(false)
{
	netplay_assert(size > 0);
	m_data = new char[size];
}

netplay_memory::netplay_memory
(
	unsigned int index,
	const std::string& name,
	void* data,
	size_t size
) :
	m_size(size), m_index(index), m_name(name), m_data((char*)data),
	m_owns_memory(false), m_generation(0), m_dirty(false)
{
	netplay_assert(data != nullptr);
	netplay_assert(size > 0);
}

netplay_memory::~netplay_memory()
{
	netplay_assert(m_data != nullptr);
	if (m_owns_memory)
	{
		delete [] m_data;
	}
}

void netplay_memory::copy_from(const netplay_memory& block)
{
	netplay_assert(m_size == block.m_size);
	memcpy(m_data, block.m_data, m_size);
	m_generation = block.m_generation;
}

unsigned char netplay_memory::checksum() const
{
	unsigned char checksum = 0;

	for (auto i = 0u; i < m_size; i++)
	{
		checksum ^= m_data[i];
	}

	return checksum;
}

std::string netplay_memory::get_debug_string()
{
	std::stringstream ss;
	ss << "memory block #" << m_index << " \"" << m_name << "\" @ " << (size_t)m_data;
	ss << " [ " << "size = " << m_size << ", gen  = " << m_generation;
	ss << ", owns_memory = " << (m_owns_memory ? "yes" : "no") << " ]";
	return ss.str();
}
