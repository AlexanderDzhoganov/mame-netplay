#ifndef MAME_NETPLAY_EMU_SOCKET_H
#define MAME_NETPLAY_EMU_SOCKET_H

// network implementation
// backend needs to be reliable and strictly ordered i.e. no skipped, lost or reordered packets

enum netplay_status
{
	NETPLAY_NO_ERR = 0,
	NETPLAY_LZMA_ERROR
};

typedef netplay_stream_reader<netplay_raw_byte_stream> netplay_socket_reader;
typedef netplay_stream_writer<netplay_memory_stream> netplay_socket_writer;

struct netplay_socket_impl;

class netplay_socket
{
public:
	netplay_socket(netplay_manager& manager);
	~netplay_socket();
	netplay_addr get_self_address() const;

	netplay_status listen(const netplay_listen_socket& listen_opts);
	netplay_status connect(const netplay_addr& address);
	netplay_status disconnect(const netplay_addr& address);
	netplay_status send(netplay_memory_stream& stream, const netplay_addr& address);
	netplay_status broadcast(netplay_memory_stream& stream);

	bool socket_connected(const netplay_addr& address);
	void socket_disconnected(const netplay_addr& address);
	void socket_data(char* data, int length, char* sender);

	static std::string addr_to_str(const netplay_addr& address);
	static netplay_addr str_to_addr(const std::string& address);
	
	netplay_manager& manager() { return m_manager; }

private:
	netplay_manager& m_manager;
	netplay_socket_impl* m_impl;
};

#endif
