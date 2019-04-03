
#pragma once

#ifndef __EMU_H__
#error Dont include this file directly; include emu.h instead.
#endif

#ifndef MAME_EMU_NETPLAY_H
#define MAME_EMU_NETPLAY_H

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

class netplay_manager
{
	friend class running_machine;
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

	attotime machine_time() const { return m_machine_time; }
	bool catching_up() const { return m_catching_up; }

	const std::vector<std::shared_ptr<netplay_peer>>& peers() const { return m_peers; }

protected:
	// called by socket implementation
	bool socket_connected(const netplay_address& address);
	void socket_disconnected(const netplay_address& address);

	// called by running_machine
	void set_machine_time(attotime machine_time) { m_machine_time = machine_time; }

	// methods accessed by save
	void create_memory_block(const std::string& name, void* data_ptr, size_t size);

	// methods accessed by ioport
	void add_input_state(std::unique_ptr<netplay_input> input_state);
	unsigned int input_freq_ms() const { return m_input_freq_ms; }

private:
	void update_host();
	void update_client();
	void store_sync();
	void load_sync();
	void send_initial_sync(const netplay_peer& peer);

	std::shared_ptr<netplay_peer> add_peer(const std::string& name, const netplay_address& address, bool self = false);
	void cleanup_inputs();

	static unsigned char memory_checksum(const netplay_blocklist& blocks);

	running_machine& m_machine;

	bool m_initialized;             // whether netplay is initialized
	bool m_debug;                   // whether debug logging for netplay is enabled
	bool m_host;                    // whether this node is the host
	netplay_address m_host_address; // address of the host
	size_t m_max_block_size;        // maximum memory block size, blocks larger than this get split up
	unsigned int m_sync_every;      // (server only) how often to sync in seconds
	unsigned int m_input_freq_ms;   // how often to sync inputs in milliseconds

	std::unique_ptr<netplay_socket> m_socket;           // network socket implementation
	std::vector<std::shared_ptr<netplay_peer>> m_peers; // connected peers

	netplay_blocklist m_active_blocks;                  // active (in-use) memory blocks by the emulator

	netplay_blocklist m_sync_blocks;                    // stale blocks used for sync
	size_t m_sync_generation;                           // the generation id of the current sync blocks
	attotime m_sync_time;                               // timestamp of last sync

	attotime m_machine_time;                            // current machine time
	bool     m_catching_up;                             // are we catching up?
};

#endif
