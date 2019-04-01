#include "emu.h"
#include "netplay_socket.h"

#ifdef EMSCRIPTEN

netplay_socket::netplay_socket(netplay_manager& manager) :
	m_manager(manager) {}

netplay_address netplay_socket::get_self_address() const
{
	return std::string("dummy");
}

netplay_status netplay_socket::listen(const netplay_listen_socket& listen_opts)
{
	m_manager.initialized();
	// this is a stub because the connection is done on the js side
	return netplay_no_err;
}

netplay_status netplay_socket::connect(const netplay_address& address)
{
	// this is a stub because the connection is done on the js side
	return netplay_no_err;
}

void netplay_socket::disconnect(const netplay_address& address)
{

}

void netplay_socket::send(const netplay_packet& packet, const netplay_address& address)
{

}

bool netplay_socket::receive(netplay_packet& out_packet, netplay_address& out_address)
{
	return false;
}

std::string netplay_socket::address_to_string(const netplay_address& address)
{
	return address;
}

#endif
