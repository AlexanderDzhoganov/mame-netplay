#ifndef __NETPLAY_SOCKET_H__
#define __NETPLAY_SOCKET_H__

// network implementation
// backend needs to be reliable and strictly ordered i.e. no skipped, lost or reordered packets

class netplay_packet;

struct netplay_listen_socket {};

enum netplay_status
{
	netplay_no_err = 0
};

class netplay_socket
{
public:
	netplay_socket(netplay_manager& manager);
	netplay_address get_self_address() const;

	netplay_status listen(const netplay_listen_socket& listen_opts);
	netplay_status connect(const netplay_address& address);
	void disconnect(const netplay_address& address);

	void send(const netplay_packet& packet, const netplay_address& address);
	bool receive(netplay_packet& out_packet, netplay_address& out_address);

	static std::string address_to_string(const netplay_address& address);

private:
	netplay_manager& m_manager;
};

#endif
