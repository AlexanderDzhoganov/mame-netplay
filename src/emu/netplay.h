
#ifndef MAME_EMU_NETPLAY_H
#define MAME_EMU_NETPLAY_H

#include "netplay/util.h"
#include "netplay/addr.h"
#include "netplay/serialization.h"
#include "netplay/socket.h"

class netplay_memory;
class netplay_peer;
struct netplay_input;
struct netplay_sync;
struct netplay_checksum;

typedef std::vector<std::shared_ptr<netplay_memory>> netplay_blocklist;
typedef unsigned int netplay_frame;

struct netplay_state
{
	netplay_frame m_frame_count;
	attotime m_timestamp;
	netplay_blocklist m_blocks;
};

typedef std::vector<std::shared_ptr<netplay_peer>> netplay_peerlist;
typedef netplay_circular_buffer<netplay_state, 5> netplay_statelist;

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
	bool catching_up() const { return m_catching_up; }
	bool input_enabled() const { return m_frame_count > m_input_delay; }
	netplay_frame frame_count() const { return m_frame_count; }
	unsigned int input_delay() const { return m_input_delay; }
	const netplay_peerlist& peers() const { return m_peers; }
	running_machine& machine() { return m_machine; }
	attotime system_time() const;
	void print_debug_info();

protected:
	// called by the socket implementation
	bool socket_connected(const netplay_addr& address);
	void socket_disconnected(const netplay_addr& address);
	void socket_data(netplay_socket_reader& reader, const netplay_addr& sender);

	// methods called by save_manager
	void create_memory_block(const std::string& module_name, const std::string& name, void* data_ptr, size_t size);

	// methods called by ioport_manager
	void send_input_state(netplay_input& input_state);
	void next_frame() { m_frame_count++; }

private:
	void recalculate_input_delay();
	void set_input_delay(unsigned int input_delay);
	bool store_state();
	void load_state(const netplay_state& state);
	bool rollback(netplay_frame before_frame);
	void send_full_sync(const netplay_peer& peer);
	void handle_host_packet(netplay_socket_reader& reader, unsigned int flags, const netplay_addr& sender);
	void handle_client_packet(netplay_socket_reader& reader, unsigned int flags, const netplay_addr& sender);
	void handle_sync(const netplay_sync& sync, netplay_socket_reader& reader, netplay_peer& peer);
	void handle_input(netplay_input& input_state, netplay_peer& peer);
	void handle_checksum(std::unique_ptr<netplay_checksum> checksum, netplay_peer& peer);

	netplay_peer& add_peer(const std::string& name, const netplay_addr& address, bool self = false);
	netplay_peer* get_peer_by_addr(const netplay_addr& address) const;

	running_machine& m_machine;

	bool m_initialized;               // whether netplay is initialized
	bool m_debug;                     // whether debug logging for netplay is enabled
	bool m_host;                      // whether this node is the host
	size_t m_max_block_size;          // maximum memory block size, blocks larger than this get split up
	unsigned int m_input_delay_min;   // minimum input delay
	unsigned int m_input_delay_max;   // maximum input delay
	unsigned int m_input_delay;       // how many frames of input delay to use, higher numbers result in less rollbacks
	unsigned int m_checksum_every;    // how often to send checksum checks
	unsigned int m_ping_every;        // how often to send pings
	unsigned int m_max_rollback;      // maximum number of frames we're allowed to rollback

	netplay_peerlist m_peers;         // connected peers

	netplay_blocklist m_memory;       // active (in-use) memory blocks by the emulator
	netplay_statelist m_states;       // saved emulator states

	bool m_catching_up;               // are we catching up?
	bool m_waiting_for_client;        // are we waiting on a client?

	bool m_initial_sync;              // (client only) whether the initial sync has completed
	bool m_rollback;                  // are we rolling back this update?
	netplay_frame m_rollback_frame;   // where to rollback to
	netplay_frame m_frame_count;      // current "frame" (update) count

	std::list<std::unique_ptr<netplay_checksum>> m_pending_checksums;

	std::unique_ptr<netplay_socket> m_socket; // network socket implementation
	attotime m_startup_time;
	attotime m_last_ping_time;
	bool m_has_ping_time;
};

#endif
