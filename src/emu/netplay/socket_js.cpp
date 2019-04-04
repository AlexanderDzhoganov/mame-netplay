// #ifdef EMSCRIPTEN

#include <deque>
#include <emscripten.h>

#include "netplay.h"
#include "netplay/serialization.h"
#include "netplay/socket.h"

struct js_packet
{
  std::vector<char> data;
  std::string address;
};

netplay_socket* netplay_socket_instance = nullptr;
std::deque<js_packet> netplay_recv_queue;

netplay_socket::netplay_socket(netplay_manager& manager) :
	m_manager(manager)
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
	// this is a stub because the connection is done on the js side
	return NETPLAY_NO_ERR;
}

void netplay_socket::disconnect(const netplay_addr& address)
{
  // NETPLAY TODO: implement this
}

void netplay_socket::send(netplay_socket_stream& stream, const netplay_addr& address)
{
  auto& data = stream.data();

  EM_ASM_ARGS({
		jsmame_netplay_packet($0, $1, $2);
  }, (unsigned int)data.data(), (unsigned int)data.size(), (unsigned int)address.m_peerid.c_str());
}

bool netplay_socket::receive(netplay_socket_stream& stream, netplay_addr& address)
{
	if (netplay_recv_queue.empty())
	{
		return false;
	}

	js_packet& packet = netplay_recv_queue.front();
	address = netplay_socket::str_to_addr(packet.address);
	stream.set_data(std::move(packet.data));
  netplay_recv_queue.pop_front();
	return true;
}

bool netplay_socket::socket_connected(const netplay_addr& address)
{
  return m_manager.socket_connected(address);
}

void netplay_socket::socket_disconnected(const netplay_addr& address)
{
  m_manager.socket_disconnected(address);
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
    netplay_recv_queue.emplace_back();
    auto& packet = netplay_recv_queue.back();
    packet.data.resize(length);
    memcpy(packet.data.data(), data, length * sizeof(char));
    packet.address = sender;
  }

  bool js_netplay_connect(char* address)
  {
    if (netplay_socket_instance != nullptr)
    {
      std::string ss(address);
      auto addr = netplay_socket::str_to_addr(ss);
      return netplay_socket_instance->socket_connected(addr);
    }

    return false;
  }

  void js_netplay_disconnect(char* address)
  {
    if (netplay_socket_instance != nullptr)
    {
      std::string ss(address);
      auto addr = netplay_socket::str_to_addr(ss);
      netplay_socket_instance->socket_disconnected(addr);
    }
  }
}

// #endif
