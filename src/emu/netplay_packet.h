#ifndef __NETPLAY_PACKET_H__
#define __NETPLAY_PACKET_H__

class netplay_memory_block;
class netplay_stream_reader;
class netplay_stream_writer;

struct netplay_handshake
{
	std::string m_name;
};

class netplay_packet
{

public:
	attotime get_timestamp() const { return m_timestamp; }
	void set_timestamp(attotime timestamp) { m_timestamp = timestamp; }

	void add_handshake(const netplay_handshake& handshake);
	netplay_handshake& get_handshake() const;
	bool has_handshake() const;

	void add_input_state(const netplay_input_state& input_state);
	std::unique_ptr<netplay_input_state> get_input_state();
	bool has_input_state() const;

	void add_sync_block(const netplay_memory_block& block);
	void copy_sync_blocks(std::vector<std::shared_ptr<netplay_memory_block>>& dest_blocks);
	size_t num_sync_blocks() const;

	size_t get_packet_size() const;

private:
	attotime m_timestamp;
};

#endif
