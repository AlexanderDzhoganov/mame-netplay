#ifndef MAME_EMU_NETPLAY_ADDR_H
#define MAME_EMU_NETPLAY_ADDR_H

#ifdef EMSCRIPTEN

struct netplay_addr
{
	friend class netplay_socket;
	bool operator==(const netplay_addr& address) const { return m_peerid == address.m_peerid; }

	protected:
	std::string m_peerid;
};

struct netplay_listen_socket {};

#else
struct netplay_addr
{
	bool operator==(const netplay_addr& address) const { return false; }
};
struct netplay_listen_socket {};
#endif

#endif
