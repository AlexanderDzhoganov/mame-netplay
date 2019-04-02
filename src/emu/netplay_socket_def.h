#ifndef MAME_EMU_NETPLAY_SOCKET_DEF_H
#define MAME_EMU_NETPLAY_SOCKET_DEF_H

#ifdef EMSCRIPTEN

struct netplay_address
{
	friend class netplay_socket;
	bool operator==(const netplay_address& address) const { return m_peerid == address.m_peerid; }

	protected:
	std::string m_peerid;
};

struct netplay_listen_socket {};

#else
struct netplay_address {};
struct netplay_listen_socket {};
#endif

#endif
