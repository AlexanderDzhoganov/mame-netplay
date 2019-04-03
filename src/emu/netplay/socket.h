#ifndef MAME_NETPLAY_EMU_SOCKET_H
#define MAME_NETPLAY_EMU_SOCKET_H

// network implementation
// backend needs to be reliable and strictly ordered i.e. no skipped, lost or reordered packets

enum netplay_status
{
	NETPLAY_NO_ERR = 0
};

typedef netplay_memory_stream netplay_socket_stream;
typedef netplay_stream_reader<netplay_memory_stream> netplay_socket_reader;
typedef netplay_stream_writer<netplay_memory_stream> netplay_socket_writer;

class netplay_socket
{
public:
	netplay_socket(netplay_manager& manager);
	netplay_addr get_self_address() const;

	netplay_status listen(const netplay_listen_socket& listen_opts);
	netplay_status connect(const netplay_addr& address);
	void disconnect(const netplay_addr& address);

	void send(netplay_socket_stream& stream, const netplay_addr& address);
	bool receive(netplay_socket_stream& stream, netplay_addr& address);

	bool socket_connected(const netplay_addr& address);
	void socket_disconnected(const netplay_addr& address);

	static std::string addr_to_str(const netplay_addr& address);
	static netplay_addr str_to_addr(const std::string& address);
	
	netplay_manager& manager() { return m_manager; }

private:
	netplay_manager& m_manager;
};

#endif
