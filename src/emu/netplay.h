
#pragma once

#ifndef __EMU_H__
#error Dont include this file directly; include emu.h instead.
#endif

#ifndef MAME_EMU_NETPLAY_H
#define MAME_EMU_NETPLAY_H

#include "netplay_util.h"
#include "netplay_socket_def.h"

#define NETPLAY_LOG(...) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }

#ifndef NO_NETPLAY_ASSERT
#define netplay_assert(COND) do { if (!(COND)) { printf("\n\nassertion failed: " #COND " (%s:%d)\n\n", __FILE__, __LINE__); exit(1); } } while(0);
#endif

class netplay_socket;
class netplay_memory;
class netplay_peer;
struct netplay_input;

typedef std::vector<std::shared_ptr<netplay_memory>> netplay_blocklist;
typedef std::vector<std::shared_ptr<netplay_peer>> netplay_peerlist;

struct netplay_state
{
	netplay_blocklist m_blocks;
	attotime m_timestamp;
	unsigned long long m_frame_count;
	int m_generation;
};

typedef netplay_circular_buffer<netplay_state, 3> netplay_statelist;

class netplay_manager
{
	friend class save_manager;
	friend class ioport_manager;
	friend class netplay_socket;

public:
	netplay_manager(running_machine& machine);

	bool initialize();
	void update();
	void print_debug_info();

	running_machine& machine() { return m_machine; }

	bool initialized() const { return m_initialized; }
	bool hosting() const { return m_host; }
	bool debug() const { return m_debug; }
	void set_debug(bool debug) { m_debug = debug; }
	bool catching_up() const { return m_catching_up; }
	bool waiting_for_client() const { return m_waiting_for_client; }

	attotime machine_time() const { return m_machine_time; }
	attotime system_time() const;
	unsigned long long frame_count() const { return m_frame_count; }

	const netplay_peerlist& peers() const { return m_peers; }
	netplay_peer* get_peer_by_addr(const netplay_addr& address) const;

protected:
	// called by socket implementation
	bool socket_connected(const netplay_addr& address);
	void socket_disconnected(const netplay_addr& address);

	// methods called by save
	void create_memory_block(const std::string& name, void* data_ptr, size_t size);

	// methods called by ioport
	void add_input_state(std::unique_ptr<netplay_input> input_state);

private:
	void update_host();
	void update_client();
	void store_state();
	void load_state(const netplay_state& state);
	bool rollback(const attotime& to_time);
	void send_full_sync(const netplay_peer& peer);

	netplay_peer& add_peer(const std::string& name, const netplay_addr& address, bool self = false);

	static unsigned char memory_checksum(const netplay_blocklist& blocks);

	running_machine& m_machine;

	bool m_initialized;                // whether netplay is initialized
	bool m_debug;                      // whether debug logging for netplay is enabled
	bool m_host;                       // whether this node is the host
	netplay_addr m_host_address;       // address of the host
	size_t m_max_block_size;           // maximum memory block size, blocks larger than this get split up
	attotime m_sync_every;             // how often to do a memory sync

	netplay_peerlist m_peers;          // connected peers

	netplay_blocklist m_active_blocks; // active (in-use) memory blocks by the emulator
	netplay_statelist m_states;        // saved emulator states

	attotime m_last_system_time;       // system time on last update
	attotime m_machine_time;           // current machine time
	unsigned long long m_frame_count;  // current netplay frame count
	bool     m_catching_up;            // are we catching up?
	bool     m_waiting_for_client;     // are we waiting on a client?

	std::unique_ptr<netplay_socket> m_socket; // network socket implementation
};

#endif
