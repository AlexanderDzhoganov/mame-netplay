// #ifdef EMSCRIPTEN

#include <emscripten.h>

#include "netplay.h"
#include "netplay/serialization.h"
#include "netplay/socket.h"

struct netplay_socket_impl
{
  std::vector<char> m_scratchpad;

  size_t compress(netplay_memory_stream& stream)
  {
    auto& data = stream.data();
    size_t orig_size = data.size();

    auto compressed_size = netplay_max_compressed_size(orig_size);
    m_scratchpad.resize(compressed_size + sizeof(size_t));
    memcpy(m_scratchpad.data(), &orig_size, sizeof(size_t));

    if(!netplay_compress(data.data(), orig_size, m_scratchpad.data() + sizeof(size_t), compressed_size))
    {
      netplay_assert("lzma compression error" == 0);
      NETPLAY_LOG("lzma compression error");
      return 0;
    }

    compressed_size += sizeof(size_t);
    return compressed_size;
  }
};

netplay_socket* netplay_socket_instance = nullptr;

netplay_socket::netplay_socket(netplay_manager& manager) : m_manager(manager)
{
  m_impl = new netplay_socket_impl();
  netplay_socket_instance = this; // pretty hacky, there's probably a better way
}

netplay_socket::~netplay_socket()
{
  delete m_impl;
}

netplay_addr netplay_socket::get_self_address() const
{
	return netplay_socket::str_to_addr("dummy");
}

netplay_status netplay_socket::listen(const netplay_listen_socket& listen_opts)
{
	// this is a stub because the connection is done on the js side
	return NETPLAY_NO_ERR;
}

netplay_status netplay_socket::connect(const netplay_addr& address)
{
  EM_ASM_ARGS({
    jsmame_netplay_connect($0);
  }, address.m_peerid.c_str());

	return NETPLAY_NO_ERR;
}

netplay_status netplay_socket::disconnect(const netplay_addr& address)
{
  auto addr = addr_to_str(address);

  EM_ASM({
    jsmame_netplay_disconnect($0);
  }, (unsigned int)addr.c_str());

	return NETPLAY_NO_ERR;
}

netplay_status netplay_socket::send(netplay_memory_stream& stream, const netplay_addr& address)
{
  auto compressed_size = m_impl->compress(stream);
  if(compressed_size == 0)
  {
    netplay_assert("lzma compression error" == 0);
    NETPLAY_LOG("lzma compression error");
    return NETPLAY_LZMA_ERROR;
  }
  
  EM_ASM_ARGS({
		jsmame_netplay_packet($0, $1, $2);
  }, (unsigned int)m_impl->m_scratchpad.data(), (unsigned int)compressed_size, (unsigned int)address.m_peerid.c_str());
  
	return NETPLAY_NO_ERR;
}

netplay_status netplay_socket::broadcast(netplay_memory_stream& stream)
{
  auto compressed_size = m_impl->compress(stream);
  if(compressed_size == 0)
  {
    netplay_assert("lzma compression error" == 0);
    NETPLAY_LOG("lzma compression error");
    return NETPLAY_LZMA_ERROR;
  }
  
  EM_ASM_ARGS({
		jsmame_netplay_broadcast($0, $1);
  }, (unsigned int)m_impl->m_scratchpad.data(), (unsigned int)compressed_size);
  
	return NETPLAY_NO_ERR;
}

bool netplay_socket::socket_connected(const netplay_addr& address)
{
  return m_manager.socket_connected(address);
}

void netplay_socket::socket_disconnected(const netplay_addr& address)
{
  m_manager.socket_disconnected(address);
}

void netplay_socket::socket_data(char* data, int length, char* sender)
{
  size_t orig_size;
  memcpy(&orig_size, data, sizeof(size_t));
 
  data += sizeof(size_t);
  length -= sizeof(size_t);

  auto& scratchpad = m_impl->m_scratchpad;
  scratchpad.resize(orig_size);

  if (!netplay_decompress(data, length, scratchpad.data(), orig_size))
  {
    NETPLAY_LOG("lzma decompression error");
    netplay_assert("lzma decompression error" == 0);
    return;
  }

  netplay_raw_byte_stream stream(scratchpad.data(), orig_size);
  netplay_socket_reader reader(stream);
  m_manager.socket_data(reader, netplay_socket::str_to_addr(sender));
}

std::string netplay_socket::addr_to_str(const netplay_addr& address)
{ 
  return address.m_peerid;
}

netplay_addr netplay_socket::str_to_addr(const std::string& address)
{
  netplay_addr addr;
  addr.m_peerid = address;
  return addr;
}

// javascript exports

extern "C" {
  void js_netplay_enqueue(char* data, int length, char* sender)
  {
    if (netplay_socket_instance == nullptr)
    {
      NETPLAY_LOG("js_netplay_enqueue() called but socket is non initialized");
      return;
    }

    netplay_socket_instance->socket_data(data, length, sender);
  }

  bool js_netplay_connect(char* address)
  {
    if (netplay_socket_instance == nullptr)
    {
      NETPLAY_LOG("js_netplay_connect() called but socket is non initialized");
      return false;
    }

    std::string ss(address);
    auto addr = netplay_socket::str_to_addr(ss);
    return netplay_socket_instance->socket_connected(addr);
  }

  void js_netplay_disconnect(char* address)
  {
    if (netplay_socket_instance == nullptr)
    {
      NETPLAY_LOG("js_netplay_disconnect() called but socket is non initialized");
      return;
    }

    std::string ss(address);
    auto addr = netplay_socket::str_to_addr(ss);
    netplay_socket_instance->socket_disconnected(addr);
  }
}

// #endif
