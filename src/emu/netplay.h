
#ifndef MAME_EMU_NETPLAY_H
#define MAME_EMU_NETPLAY_H

#include "netplay/util.h"
#include "netplay/memory.h"

typedef std::vector<std::shared_ptr<netplay_memory>> netplay_blocklist;

#include "netplay/addr.h"
#include "netplay/serialization.h"
#include "netplay/socket.h"
#include "netplay/input_state.h"
#include "netplay/packet.h"

class netplay_memory;
class netplay_peer;
struct netplay_handshake;
struct netplay_input;
struct netplay_sync;

struct netplay_state
{
	netplay_frame m_frame_count;
	netplay_blocklist m_blocks;

	netplay_state() : m_frame_count(0) {}

	unsigned int checksum()
	{
		unsigned int checksum = 0;
		for(auto& block : m_blocks)
			checksum ^= block->checksum();
		return checksum;
	}

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.write(m_frame_count);

		for (auto& block : m_blocks)
			block->serialize(writer);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.read(m_frame_count);

		for (auto& block : m_blocks)
			block->deserialize(reader);
	}
};

typedef std::vector<std::shared_ptr<netplay_peer>> netplay_peerlist;

struct netplay_stats
{
	unsigned int m_syncs;
	unsigned int m_sync_total_bytes;
	unsigned int m_rollbacks;
	unsigned int m_max_latency;
	unsigned int m_avg_latency_sum;
	unsigned int m_avg_latency_n;
	unsigned int m_packets_received;
	unsigned int m_packets_sent;
	unsigned int m_waited_for_inputs;
};

class netplay_manager
{
	friend class ioport_manager;
	friend class netplay_socket;

public:
	static netplay_manager* m_instance;
	static netplay_manager* instance() { return m_instance; }

	netplay_manager(running_machine& machine);
	~netplay_manager();

	bool initialize();
	void update();

	bool initialized() const { return m_initialized; }
	bool catching_up() const { return m_catching_up; }
	netplay_frame frame_count() const { return m_frame_count; }
	unsigned int input_delay() const { return m_input_delay; }
	const netplay_peerlist& peers() const { return m_peers; }
	running_machine& machine() { return m_machine; }
	attotime system_time() const;
	netplay_peer* my_peer() const;

	void save_game();
	void load_game(uintptr_t bytes_ptr, int size);
private:
	unsigned int calculate_input_delay();
	void recalculate_input_delay();
	void send_sync(bool full_sync);
	void send_ping();

	void store_state();
	void load_state(const netplay_state& state);
	void rollback();
	void simulate_until(netplay_frame frame_index);

	void handle_host_packet(netplay_socket_reader& reader, unsigned char flags, netplay_peer& peer);
	void handle_client_packet(netplay_socket_reader& reader, unsigned char flags, netplay_peer& peer);
	void handle_handshake(const netplay_handshake& handshake, netplay_peer& peer);
	void handle_sync(const netplay_sync& sync, netplay_socket_reader& reader, netplay_peer& peer);
	void handle_inputs(netplay_socket_reader& reader, netplay_peer& peer);
	void handle_ping(netplay_ping& ping, netplay_peer& peer);

	netplay_peer& add_peer(unsigned char peerid, const netplay_addr& address, bool self = false);
	netplay_peer* get_peer(const netplay_addr& address) const;
	netplay_peer* get_peer(unsigned char peerid) const;
	bool wait_for_connection();
	void set_input_delay(unsigned int input_delay);
	bool can_save();
	unsigned int memory_checksum();
	unsigned int num_frames_to_wait();
	bool should_send_inputs();
	float avg_peer_latency();

	void create_memory_block(state_entry& entry);
	void write_packet_header(netplay_socket_writer& writer, unsigned char flags);
	bool read_packet_header(netplay_socket_reader& reader, unsigned char& flags, netplay_peer& sender);

	void print_stats() const;

	// called by the socket implementation
	bool socket_connected(const netplay_addr& address);
	bool host_socket_connected(const netplay_addr& address);
	bool client_socket_connected(const netplay_addr& address);
	void socket_disconnected(const netplay_addr& address);
	void socket_data(netplay_socket_reader& reader, const netplay_addr& sender);

	// methods called by ioport_manager
	void send_input_state(netplay_frame frame_index);

	running_machine& m_machine;

	bool m_initialized;             // whether netplay is initialized
	bool m_debug;                   // whether debug logging for netplay is enabled
	bool m_host;                    // whether this node is the host
	std::string m_name;
	netplay_addr m_host_address;    // the network address of the host
	unsigned int m_input_delay;     // how many frames of input delay to use. higher numbers result in less rollbacks
	unsigned int m_max_rollback;    // maximum number of frames we're allowed to rollback

	netplay_peerlist m_peers;       // connected peers

	netplay_blocklist m_memory;     // memory blocks in-use by the emulator
	netplay_state m_checkpoint;     // last stored state
	netplay_state m_snapshot;       // last known good state acknowledged between all peers

	unsigned int m_sync_generation; // we increment this every time we do a sync
	netplay_frame m_frame_count;    // current frame (update) count

	bool m_catching_up;             // are we catching up?
	bool m_waiting_for_connection;

	netplay_stats m_stats;          // various collected stats
	netplay_delay m_next_input_delay;
	unsigned int m_input_delay_backoff;
	unsigned char m_next_peerid;
	bool m_desynced;
	unsigned int m_sync_cooldown;

	attotime m_host_time;

	std::unique_ptr<netplay_socket> m_socket; // network socket implementation
};

#endif
