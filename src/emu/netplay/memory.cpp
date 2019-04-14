#include <string>
#include <sstream>

#include "util/hash.h"
#include "netplay/util.h"
#include "netplay.h"
#include "netplay/memory.h"

#define CRC32(DATA, LENGTH) util::crc32_creator::simple(DATA, LENGTH).m_raw

//-------------------------------------------------
// netplay_memory
//-------------------------------------------------

netplay_memory::netplay_memory
(
	unsigned int index,
	const std::string& module_name,
	const std::string& name,
	size_t size
) :
	m_size(size),
	m_index(index),
	m_module_hash(CRC32(module_name.c_str(), module_name.length())),
	m_module_name(name),
	m_name(name),
	m_data(nullptr),
	m_owns_memory(true)
{
	netplay_assert(size > 0);
	m_data = new char[size];
}

netplay_memory::netplay_memory
(
	unsigned int index,
	const std::string& module_name,
	const std::string& name,
	void* data,
	size_t size
) :
	m_size(size),
	m_index(index),
	m_module_hash(CRC32(module_name.c_str(), module_name.length())),
	m_module_name(name),
	m_name(name),
	m_data((char*)data),
	m_owns_memory(false)
{
	netplay_assert(data != nullptr);
	netplay_assert(size > 0);
}

netplay_memory::~netplay_memory()
{
	netplay_assert(m_data != nullptr);

	if (m_owns_memory)
		delete [] m_data;
}

void netplay_memory::copy_from(const netplay_memory& block)
{
	netplay_assert(m_size == block.m_size);
	memcpy(m_data, block.m_data, m_size);
}

unsigned int netplay_memory::checksum()
{
	m_checksum = CRC32(m_data, m_size);
	return m_checksum;
}

std::string netplay_memory::debug_string()
{
	std::stringstream ss;
	ss << "memory block #" << m_index << " \"" << m_name << "\" @ " << (size_t)m_data;
	ss << " [ " << "size = " << m_size << ", owns_memory = " << (m_owns_memory ? "yes" : "no") << " ]";
	return ss.str();
}
