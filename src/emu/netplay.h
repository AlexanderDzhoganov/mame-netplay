
#pragma once

#ifndef __EMU_H__
#error Dont include this file directly; include emu.h instead.
#endif

#ifndef __NETPLAY_H__
#define __NETPLAY_H__

#define NETPLAY_LOG(...) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }

#ifdef EMSCRIPTEN
typedef std::string netplay_address;
#else
typedef std::string netplay_address;
#endif

class netplay_socket;
class netplay_peer;
class netplay_memory_block;
struct netplay_input_state;

class netplay_manager
{
	friend class running_machine;
	friend class save_manager;
	friend class ioport_manager;

public:
	netplay_manager(running_machine& machine);

	bool initialize();
	void update();
	void print_debug_info();

	running_machine& machine() { return m_machine; }

	bool initialized() const { return m_initialized; }
	bool hosting() const { return m_hosting; }
	bool debug() const { return m_debug; }
	void set_debug(bool debug) { m_debug = debug; }

	attotime machine_time() const { return m_machine_time; }
	
	const std::vector<std::shared_ptr<netplay_peer>>& get_peers() const { return m_peers; }

protected:
	bool socket_connected(const netplay_address& address);
	void socket_disconnected(const netplay_address& address);
	void set_machine_time(attotime machine_time) { m_machine_time = machine_time; }
	void create_memory_block(const std::string& name, void* data_ptr, size_t size);
	void add_input_state(std::unique_ptr<netplay_input_state> input_state);

private:
	void update_host();
	void update_client();
	void store_sync_point();
	void load_sync_point();
	void add_peer(const std::string& name, const netplay_address& address, bool self = false);
	void cleanup_inputs();

	static unsigned char memory_checksum(const std::vector<std::shared_ptr<netplay_memory_block>>& blocks);

	bool m_initialized;                                // whether netplay is initialized
	bool m_debug;                                      // whether debug logging for netplay is enabled
	bool m_hosting;                                    // whether this node is the host
	size_t m_max_block_size;                           // maximum memory block size, blocks larger than this get split up
	unsigned int m_sync_every;                         // (server only) how often to sync in seconds

	running_machine& m_machine;
	std::unique_ptr<netplay_socket> m_socket;
	std::vector<std::shared_ptr<netplay_peer>> m_peers;

	std::vector<std::shared_ptr<netplay_memory_block>> m_active_blocks; // active (in-use) memory blocks by the emulator

	std::vector<std::shared_ptr<netplay_memory_block>> m_sync_blocks;   // stale blocks used for sync
	size_t m_sync_generation;                          // the generation id of the current sync blocks
	attotime m_sync_time;                              // timestamp of last sync

	attotime m_machine_time;                           // current machine time
};

#endif
