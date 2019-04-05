// #ifdef EMSCRIPTEN

#include <deque>
#include <emscripten.h>

#include "netplay.h"
#include "netplay/serialization.h"
#include "netplay/socket.h"

netplay_socket* netplay_socket_instance = nullptr;

netplay_socket::netplay_socket(netplay_manager& manager) : m_manager(manager)
{
  netplay_socket_instance = this; // pretty hacky, there's probably a better way
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
  EM_ASM({
    jsmame_netplay_connect();
  });

	return NETPLAY_NO_ERR;
}

netplay_status netplay_socket::disconnect(const netplay_addr& address)
{
  // NETPLAY TODO: implement this on the js side

  /*EM_ASM({
    jsmame_netplay_disconnect($0);
  }, (unsigned int)address.c_str());*/

	return NETPLAY_NO_ERR;
}

netplay_status netplay_socket::send(netplay_memory_stream& stream, const netplay_addr& address)
{
  auto& data = stream.data();

  EM_ASM_ARGS({
		jsmame_netplay_packet($0, $1, $2);
  }, (unsigned int)data.data(), (unsigned int)data.size(), (unsigned int)address.m_peerid.c_str());
  
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
  netplay_raw_byte_stream stream(data, length);
  netplay_socket_reader reader(stream);

  auto addr = netplay_socket::str_to_addr(sender);
  m_manager.socket_data(reader, addr);
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
