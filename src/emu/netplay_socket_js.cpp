// #ifdef EMSCRIPTEN

#include <deque>
#include <emscripten/emscripten.h>

#include "emu.h"
#include "netplay_serialization.h"
#include "netplay_socket.h"

struct js_packet
{
  std::vector<char> data;
  std::string address;
};

netplay_socket* netplay_socket_instance = nullptr;
std::deque<js_packet> netplay_send_queue;
std::deque<js_packet> netplay_recv_queue;

netplay_socket::netplay_socket(netplay_manager& manager) :
	m_manager(manager)
{
  netplay_socket_instance = this; // pretty hacky, there's probably a better way
}

netplay_address netplay_socket::get_self_address() const
{
	return netplay_socket::str_to_addr("dummy");
}

netplay_status netplay_socket::listen(const netplay_listen_socket& listen_opts)
{
	// this is a stub because the connection is done on the js side
	return NETPLAY_NO_ERR;
}

netplay_status netplay_socket::connect(const netplay_address& address)
{
	// this is a stub because the connection is done on the js side
	return NETPLAY_NO_ERR;
}

void netplay_socket::disconnect(const netplay_address& address)
{
  // NETPLAY TODO: implement this
}

void netplay_socket::send(netplay_socket_stream& stream, const netplay_address& address)
{
	netplay_send_queue.emplace_back();
  js_packet& packet = netplay_send_queue.back();
	packet.data = stream.extract_data();
  packet.address = netplay_socket::addr_to_str(address);
}

bool netplay_socket::receive(netplay_socket_stream& stream, netplay_address& address)
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

std::string netplay_socket::addr_to_str(const netplay_address& address)
{ 
  return address.m_peerid;
}

netplay_address netplay_socket::str_to_addr(const std::string& address)
{
  netplay_address addr;
  addr.m_peerid = address;
  return addr;
}

// javascript exports

extern "C" {
  int js_netplay_get_next(char* data, char* address)
  {
    if (netplay_send_queue.empty())
    {
      return 0;
    }

    js_packet& packet = netplay_send_queue.front();

    memcpy(data, packet.data.data(), packet.data.size() * sizeof(char));

    memcpy(address, packet.address.c_str(), packet.address.length());
    address[packet.address.length()] = 0;

    int length = (int)packet.data.size();
    netplay_send_queue.pop_front();

    return length;
  }

  void js_netplay_enqueue(char* data, int length, char* sender)
  {
    js_packet packet;

    packet.data.resize(length);
    memcpy(packet.data.data(), data, length * sizeof(char));
    packet.address = sender;
    netplay_recv_queue.push_back(packet);
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
