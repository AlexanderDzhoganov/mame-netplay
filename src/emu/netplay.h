
#ifndef MAME_EMU_NETPLAY_H
#define MAME_EMU_NETPLAY_H

#include "netplay/util.h"
#include "netplay/memory.h"

typedef std::vector<std::shared_ptr<netplay_memory>> netplay_blocklist;

#include "netplay/packet.h"
#include "netplay/addr.h"
#include "netplay/serialization.h"
#include "netplay/socket.h"

class netplay_memory;
class netplay_peer;
struct netplay_handshake;
struct netplay_input;
struct netplay_sync;
struct netplay_checksum;

struct netplay_state
{
	netplay_frame m_frame_count;
	netplay_blocklist m_blocks;

	unsigned int checksum()
	{
		unsigned int checksum = 0;
		for(auto& block : m_blocks)
			checksum ^= block->checksum();
		return checksum;
	}
};

typedef std::vector<std::shared_ptr<netplay_peer>> netplay_peerlist;
typedef netplay_circular_buffer<netplay_state, 3> netplay_statelist;
typedef netplay_circular_buffer<netplay_checksum, 20> netplay_checksums;

enum netplay_sync_reason
{
	NETPLAY_SYNC_INITIAL = 0,
	NETPLAY_SYNC_CHECKSUM_ERROR,
	NETPLAY_SYNC_RESYNC,
	NETPLAY_SYNC_END
};

struct netplay_stats
{
	unsigned int m_syncs;
	unsigned int m_sync_total_bytes;
	unsigned int m_rollback_success;
	unsigned int m_rollback_fail;
	unsigned int m_max_latency;
	unsigned int m_packets_received;
	unsigned int m_packets_sent;
};

class netplay_manager
{
	friend class save_manager;
	friend class ioport_manager;
	friend class netplay_socket;

public:
	netplay_manager(running_machine& machine);

	bool initialize();
	void update();

	bool initialized() const { return m_initialized; }
	bool waiting() const { return m_catching_up || m_waiting_for_peer || m_waiting_for_inputs; }
	netplay_frame frame_count() const { return m_frame_count; }
	unsigned int input_delay() const { return m_input_delay; }
	const netplay_peerlist& peers() const { return m_peers; }
	running_machine& machine() { return m_machine; }
	attotime system_time() const;
	void print_stats() const;

private:
	void update_simulation();
	void recalculate_input_delay();
	void update_checksum_history();
	void process_checksums();
	void send_checksums();
	void send_sync(const netplay_peer& peer, netplay_sync_reason reason);

	bool store_state();
	void load_state(const netplay_state& state);
	bool rollback(netplay_frame before_frame);

	void handle_host_packet(netplay_socket_reader& reader, unsigned char flags, netplay_peer& peer);
	void handle_client_packet(netplay_socket_reader& reader, unsigned char flags, netplay_peer& peer);
	void handle_handshake(const netplay_handshake& handshake, const netplay_addr& address);
	void handle_sync(const netplay_sync& sync, netplay_socket_reader& reader, netplay_peer& peer);
	void handle_inputs(netplay_input& input_state, netplay_peer& peer);
	void handle_checksum(const netplay_checksum& checksum, netplay_peer& peer);

	netplay_peer& add_peer(const std::string& name, const netplay_addr& address, bool self = false);
	netplay_peer* get_peer_by_addr(const netplay_addr& address) const;
	bool peer_inputs_available() const;
	void create_memory_block(const std::string& module_name, const std::string& name, void* data_ptr, size_t size);

protected:
	// called by the socket implementation
	bool socket_connected(const netplay_addr& address);
	void socket_disconnected(const netplay_addr& address);
	void socket_data(netplay_socket_reader& reader, const netplay_addr& sender);

	// methods called by ioport_manager
	void send_input_state(netplay_input& input_state);
	void next_frame() { m_frame_count++; }

private:
	running_machine& m_machine;

	bool m_initialized;             // whether netplay is initialized
	bool m_debug;                   // whether debug logging for netplay is enabled
	bool m_host;                    // whether this node is the host
	size_t m_max_block_size;        // maximum memory block size. blocks larger than this get split up
	unsigned int m_input_delay_min; // minimum input delay
	unsigned int m_input_delay_max; // maximum input delay
	unsigned int m_input_delay;     // how many frames of input delay to use. higher numbers result in less rollbacks
	unsigned int m_checksum_every;  // how often to send checksum checks
	unsigned int m_ping_every;      // how often to send pings
	unsigned int m_max_rollback;    // maximum number of frames we're allowed to rollback

	netplay_peerlist m_peers;       // connected peers

	netplay_blocklist m_memory;     // memory blocks in-use by the emulator
	netplay_statelist m_states;     // saved emulator states
	netplay_state m_good_state;     // last known good state acknowledged between all peers

	unsigned int m_sync_generation; // we increment this every time we do a sync
	netplay_frame m_frame_count;    // current frame (update) count

	bool m_catching_up;             // are we catching up?
	bool m_waiting_for_peer;        // are we waiting on a peer to sync?
	bool m_waiting_for_inputs;      // are we waiting for inputs?

	bool m_has_ping_time;           // whether we have a buffered ping request
	attotime m_last_ping_time;      // system time of the buffered ping request

	netplay_stats m_stats;          // various collected stats
	netplay_checksums m_checksums;  // pending checksums
	netplay_checksums m_checksums_history;
	netplay_set_delay m_set_delay;

	std::unique_ptr<netplay_socket> m_socket; // network socket implementation
};

#endif
