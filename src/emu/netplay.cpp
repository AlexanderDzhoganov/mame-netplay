#include <string>
#include <sstream>
#include <unistd.h>
#include <memory>

#include "emu.h"
#include "emuopts.h"
#include "ui/uimain.h"

#ifdef EMSCRIPTEN
#include <emscripten.h>
#endif

#include "netplay.h"
#include "netplay/memory.h"
#include "netplay/input_state.h"
#include "netplay/packet.h"
#include "netplay/peer.h"
#include "netplay/module_blacklist.h"

#define BEGIN_PACKET(FLAGS)                         \
	netplay_memory_stream stream;                     \
	netplay_socket_writer writer(stream);             \
	netplay_packet_write(writer, FLAGS, m_sync_generation);

#define END_PACKET(ADDR) do {   \
	m_socket->send(stream, ADDR); \
	m_stats.m_packets_sent++;     \
} while(0);

/* * *   TODO   * * *
 *
 * - posix & win32 sockets
 * - support more than 1 client
 * - input remapping
 *
 */

//-------------------------------------------------
// netplay_manager
//-------------------------------------------------

// const attotime netplay_frametime(0, HZ_TO_ATTOSECONDS(60));

netplay_manager::netplay_manager(running_machine& machine) : 
	 m_machine(machine),
	 m_initialized(false),
	 m_sync_generation(0),
	 m_catching_up(false),
	 m_waiting_for_client(false),
	 m_initial_sync(false),
	 m_rollback(false),
	 m_rollback_frame(0),
	 m_frame_count(1),
	 m_has_ping_time(false),
	 m_last_ping_time(0, 0),
	 m_startup_time(system_time())
{
	auto& opts = m_machine.options();
	auto host_address = opts.netplay_host();

	// configuration options
	m_debug = opts.netplay_debug();               // netplay debug printing
	m_host = strlen(host_address) == 0;           // is this node the host
	m_max_block_size = opts.netplay_block_size(); // 1kb max block size
	m_input_delay_min = 2;                        // minimum input delay
	m_input_delay_max = 20;                       // maximum input delay
	m_input_delay = 5;                            // use N frames of input delay
	m_checksum_every = 71;                        // checksum every N frames
	m_ping_every = 5;                             // ping every N frames
	m_max_rollback = 3;                           // max rollback of N frames

	m_states.set_capacity(m_max_rollback + 1);
	for (auto i = 0; i < m_states.capacity(); i++)
		m_states.push_back(netplay_state());
}

bool netplay_manager::initialize()
{
	netplay_assert(!m_initialized);
	NETPLAY_LOG("initializing netplay");

	m_socket = std::make_unique<netplay_socket>(*this);

	auto self_address = m_socket->get_self_address();
	add_peer(m_host ? "server" : "client", self_address, true);

	if (m_host)
	{
		netplay_listen_socket listen_socket;
		if (m_socket->listen(listen_socket) != NETPLAY_NO_ERR)
		{
			NETPLAY_LOG("failed to open listen socket");
			return false;
		}
	}
	else
	{
		auto host_string = m_machine.options().netplay_host();
		netplay_addr address = netplay_socket::str_to_addr(host_string);
		if (m_socket->connect(address) != NETPLAY_NO_ERR)
		{
			NETPLAY_LOG("socket failed to connect");
			return false;
		}
	}

	m_initialized = true;
	return true;
}

void netplay_manager::update()
{
	netplay_assert(m_initialized);

	auto time_since_startup = system_time() - m_startup_time;

	if (m_waiting_for_client ||
	    time_since_startup.seconds() <= 1.0 ||
	    machine().scheduled_event_pending() ||
	    machine().paused())
	{
		return;
	}

	update_simulation();

	if (m_host)
		update_host();
	else
		update_client();
}

void netplay_manager::update_simulation()
{
	auto sim_start_time = system_time();

	auto current_frame = m_frame_count;
	while (m_frame_count == current_frame)
		machine().scheduler().timeslice();

	auto sim_duration = (system_time() - sim_start_time).as_double();

	static bool perf_warning = true;
	if (perf_warning && m_startup_time.seconds() > 10 && sim_duration > (1.0 / 60.0))
	{
		NETPLAY_LOG("(WARNING) simulation is taking longer than real time");
		NETPLAY_LOG("(WARNING) make sure you can run the game at full speed");
		perf_warning = false;
	}

	if (machine().scheduler().can_save())
		store_state();

	if (m_debug && m_frame_count % 3600 == 0)
	{
		print_stats();
		memset(&m_stats, 0, sizeof(netplay_stats));
	}

	if (m_rollback && !rollback(m_rollback_frame) && m_host)
	{
		for(auto& peer : m_peers)
		{
			if (peer->self())
				continue;

			send_sync(*peer, NETPLAY_SYNC_ROLLBACK_FAILED);
		}
	}

	m_rollback = false;
}

void netplay_manager::update_host()
{
	if (m_frame_count % 120 == 0)
		recalculate_input_delay();

	if (m_frame_count % m_checksum_every == 0)
	{
		auto& state = m_states.newest();

		netplay_checksum checksum;
		checksum.m_frame_count = m_frame_count;
		checksum.m_checksums.resize(state.m_blocks.size());

		for (auto i = 0u; i < state.m_blocks.size(); i++)
			checksum.m_checksums[i] = state.m_blocks[i]->checksum();

		m_checksums_history.push_back(checksum);
	}

	for (auto& checksum : m_checksums)
	{
		if (checksum.m_processed || checksum.m_frame_count >= m_frame_count)
			continue;

		netplay_assert(m_peers.size() >= 2);
		handle_checksum(checksum, *m_peers[1]);
		checksum.m_processed = true;
	}
}

void netplay_manager::update_client()
{
	if (m_frame_count % m_checksum_every == 0)
	{
		auto& state = m_states.newest();
		auto& blocks = state.m_blocks;

		BEGIN_PACKET(NETPLAY_CHECKSUM);
			netplay_checksum checksum;
			checksum.m_frame_count = state.m_frame_count;
			checksum.m_checksums.resize(blocks.size());

			for (auto i = 0; i < blocks.size(); i++)
				checksum.m_checksums[i] = blocks[i]->checksum();

			checksum.serialize(writer);
		END_PACKET(m_peers[1]->address());
	}
}

void netplay_manager::recalculate_input_delay()
{
	auto target_latency = 0.0;

	// find the peer with the highest latency
	for (auto& peer : m_peers)
	{
		if (peer->self())
			continue;
		
		auto avg_latency = peer->average_latency();
		auto highest_latency = peer->highest_latency();
		if (highest_latency > m_stats.m_max_latency)
		{
			m_stats.m_max_latency = (unsigned int)highest_latency;
		}

		// if the highest latency is more than 4/3rds the average latency
		// take the highest latency as a target
		if ((4.0 / 3.0) * avg_latency < highest_latency)
		{
			avg_latency = highest_latency;
		}

		target_latency = std::max(target_latency, avg_latency);
	}

	// adjust the input delay
	auto calculated_delay = (unsigned int)(target_latency / (1000.0 / 60.0)) + 1;

	auto input_delay = m_input_delay;
	if (calculated_delay + 1 < input_delay)
		input_delay--;
	else if (calculated_delay > input_delay)
		input_delay++;

	input_delay = std::max(m_input_delay_min, std::min(input_delay, m_input_delay_max));
	set_input_delay(input_delay);
}

void netplay_manager::set_input_delay(unsigned int input_delay)
{
	if (m_input_delay == input_delay)
		return;

	m_input_delay = input_delay;
	NETPLAY_LOG("setting input delay to %d frames", m_input_delay);

	for(auto& peer : m_peers)
	{
		if (peer->self())
			continue;

		BEGIN_PACKET(NETPLAY_SET_DELAY);
			writer.write(m_input_delay);
		END_PACKET(peer->address());
	}
}

// create the next sync point
// copy over data from active blocks to sync blocks
bool netplay_manager::store_state()
{
	if (!machine().scheduler().can_save())
	{
		NETPLAY_LOG("(WARNING) cannot store_state() because scheduler().can_save() == false");
		return false;
	}

	// move to the next buffer in the states list
	m_states.advance(1);

	auto& state = m_states.newest();
	auto& blocks = state.m_blocks;

	// tell devices to save their memory
	machine().save().dispatch_presave();

	netplay_assert(blocks.size() == m_memory.size());

	// copy over the blocks from active memory to the new state
	for (auto i = 0; i < m_memory.size(); i++)
	{
		blocks[i]->copy_from(*m_memory[i]);
	}

	// record metadata
	state.m_timestamp = machine().time();
	state.m_frame_count = m_frame_count;

	return true;
}

// load the latest sync point
// copy over data from sync blocks to active blocks
void netplay_manager::load_state(const netplay_state& state)
{
	netplay_assert(state.m_blocks.size() == m_memory.size());

	machine().scheduler().set_basetime(state.m_timestamp);
	m_frame_count = state.m_frame_count;

	for (auto i = 0; i < m_memory.size(); i++)
		m_memory[i]->copy_from(*state.m_blocks[i]);

	// tell devices to load their memory
	machine().save().dispatch_postload();
}

// performs a rollback which first restores the latest sync state we have
// then advances the simulation to the current time while replaying all buffered inputs since then
bool netplay_manager::rollback(netplay_frame before_frame)
{
	netplay_assert(before_frame <= m_frame_count); // impossible to go to the future

	if (before_frame + m_max_rollback < m_frame_count)
	{
		m_stats.m_rollback_fail++;
		return false;
	}

	// store the current time, we'll advance the simulation to it later
	auto start_frame = m_frame_count;

	netplay_state* rollback_state = nullptr;
	netplay_frame state_frame = 0;

	// find the newest state we can rollback to
	for (auto& state : m_states)
	{
		if (state.m_frame_count > before_frame)
			continue;

		if (state.m_frame_count >= state_frame)
		{
			rollback_state = &state;
			state_frame = state.m_frame_count;
		}
	}

	if (rollback_state == nullptr)
	{
		// the given time is too far in the past and there is no state to rollback to
		// bail out and let the caller figure it out
		m_stats.m_rollback_fail++;
		return false;
	}

	// restore the machine to the chosen state
	load_state(*rollback_state);

	// let the emulator know we need to catch up
	// this should disable sound and video updates during the simulation loop below
	m_catching_up = true;

	// run the emulator loop, this will replay any new inputs that we have received since
	while (m_frame_count < start_frame)
		machine().scheduler().timeslice();

	m_catching_up = false;
	m_stats.m_rollback_success++;
	
	/*if (m_debug)
	{
		auto duration = system_time() - start_time;
		NETPLAY_LOG("rollback from %llu to %llu (took %dms)",
			start_frame, before_frame, (int)(duration.as_double() * 1000));
	}*/

	return true;
}

// sends a full sync to the selected peer
// this is slow and expensive and is used as a last resort in case of a desync
// also used to initialize newly connected peers
void netplay_manager::send_sync(const netplay_peer& peer,	netplay_sync_reason reason)
{
	m_stats.m_full_syncs[reason]++;

	m_waiting_for_client = true;
	m_rollback = false;
	m_sync_generation++;
	m_checksums.clear();

	const auto& state = m_states.newest();

	BEGIN_PACKET(NETPLAY_SYNC);
		netplay_sync sync;
		sync.m_timestamp = state.m_timestamp;
		sync.m_frame_count = state.m_frame_count;
		sync.m_input_delay = m_input_delay;
		sync.serialize(writer);

		for (auto i = 0; i < state.m_blocks.size(); i++)
		{
			auto& block = state.m_blocks[i];
			auto& good_block = m_good_state.m_blocks[i];

			// skip any blocks with the same checksum in the known good state
			// except if we're doing an initial sync, then send everything
			if (reason != NETPLAY_SYNC_INITIAL && block->checksum() == good_block->checksum())
				continue;

			// update the good state with the new memory contents
			good_block->copy_from(*block);

			// add the block to the packet
			netplay_packet_add_block(writer, *block);
		}

		m_stats.m_sync_total_bytes += writer.stream().cursor();
	END_PACKET(peer.address());
}

void netplay_manager::handle_host_packet(netplay_socket_reader& reader, unsigned int flags, const netplay_addr& sender)
{
	if (flags & NETPLAY_HANDSHAKE)
	{
		netplay_handshake handshake;
		handshake.deserialize(reader);
		handle_handshake(handshake, sender);
		return;
	}

	if (flags & NETPLAY_SYNC)
	{
		m_waiting_for_client = false;
		return;
	}
	
	auto peer = get_peer_by_addr(sender);
	
	// if we just resynced the client and she hasn't acknowledged yet
	// skip all packets because they contain old data
	if (m_waiting_for_client || peer == nullptr)
	{
		return;
	}

	if (flags & NETPLAY_INPUTS)
	{
		auto& input = peer->get_next_input_buffer();
		input.deserialize(reader);
		handle_inputs(input, *peer);

		// we piggyback pings on inputs packets

		if (flags & NETPLAY_PONG)
		{
			attotime ping_time;
			reader.read(ping_time);

			// multiply by 1000 to get milliseconds
			auto latency = (system_time() - ping_time).as_double() * 1000.0;

			// clamp the latency to a reasonable value (1ms-250ms) to remove outliers
			latency = std::max(1.0, std::min(latency, 250.0));

			// add new latency measurement for this peer
			peer->add_latency_measurement(latency);
		}

		return;
	}

	if (flags & NETPLAY_CHECKSUM)
	{
		netplay_checksum checksum;
		checksum.deserialize(reader);

		if (checksum.m_frame_count >= m_frame_count)
			m_checksums.push_back(checksum);
		else
			handle_checksum(checksum, *peer);
	}
	
	if(flags & NETPLAY_SET_DELAY)
	{
		reader.read(m_input_delay);
		NETPLAY_LOG("received new input delay '%d'", m_input_delay);
		return;
	} 
}

void netplay_manager::handle_client_packet(netplay_socket_reader& reader, unsigned int flags, const netplay_addr& sender)
{
	auto peer = get_peer_by_addr(sender);
	if (peer == nullptr)
		return;

	if (flags & NETPLAY_SYNC)
	{
		netplay_sync sync;
		sync.deserialize(reader);
		handle_sync(sync, reader, *peer);
		return;
	}

	if (flags & NETPLAY_INPUTS)
	{
		auto& input = peer->get_next_input_buffer();
		input.deserialize(reader);
		handle_inputs(input, *peer);

		// pings piggyback on input packets
		if (flags & NETPLAY_PING)
		{
			reader.read(m_last_ping_time);
			m_has_ping_time = true;
		}

		return;
	}

	if(flags & NETPLAY_SET_DELAY)
	{
		reader.read(m_input_delay);
		NETPLAY_LOG("received new input delay '%d'", m_input_delay);
		return;
	}
}

void netplay_manager::handle_handshake(const netplay_handshake& handshake, const netplay_addr& address)
{
	// add the peer to our list and send the initial sync
	auto& peer = add_peer(handshake.m_name, address);
	
	if (m_debug)
		NETPLAY_LOG("initial sync with %s at %d", peer.name().c_str(), m_frame_count);

	if (machine().scheduler().can_save())
		store_state();

	send_sync(peer, NETPLAY_SYNC_INITIAL);
}

void netplay_manager::handle_sync(const netplay_sync& sync, netplay_socket_reader& reader, netplay_peer& peer)
{
	if (m_debug)
		NETPLAY_LOG("received sync: frame = %d, size = %lukb", sync.m_frame_count, reader.stream().size() / 1024);

	m_sync_generation++;
	m_input_delay = sync.m_input_delay;

	m_good_state.m_timestamp = sync.m_timestamp;
	m_good_state.m_frame_count = sync.m_frame_count;

	// copy the memory blocks over
	netplay_packet_copy_blocks(reader, m_good_state.m_blocks);

	// restore the machine state
	load_state(m_good_state);

	// acknowledge that we have caught up
	BEGIN_PACKET(NETPLAY_SYNC);
	END_PACKET(peer.address());

	m_initial_sync = true;
	m_rollback = false; // make sure we don't rollback after a sync
}

// handle inputs coming over the network
void netplay_manager::handle_inputs(netplay_input& input_state, netplay_peer& peer)
{
	auto effective_frame = input_state.m_frame_index + m_input_delay;

	// if the input is in the future it means we're falling behind, record it and move on
	if (effective_frame > m_frame_count)
		return;

	// get any predicted input we used in place of this one
	auto predicted_input = peer.get_predicted_inputs_for(effective_frame);

	// test if both the actual and predicted inputs are the same
	// if the predicted inputs differ then we do a rollback to just before the input time
	if (predicted_input == nullptr || *predicted_input != input_state)
	{
		if (!m_rollback)
		{
			m_rollback = true;
			m_rollback_frame = effective_frame;
		}
		else if(m_rollback_frame > effective_frame)
		{
			m_rollback_frame = effective_frame;
		}
	}
}

void netplay_manager::handle_checksum(const netplay_checksum& checksum, netplay_peer& peer)
{
	bool resync = false;
	bool checksum_found = false;
	
	auto& blocks = m_states.newest().m_blocks;
	for (auto& my_checksum : m_checksums_history)
	{
		if (my_checksum.m_frame_count != checksum.m_frame_count)
			continue;

		checksum_found = true;

		netplay_assert(my_checksum.m_checksums.size() == checksum.m_checksums.size());
		netplay_assert(blocks.size() == checksum.m_checksums.size());

		for (auto i = 0; i < blocks.size(); i++)
		{
			auto module_hash = blocks[i]->module_hash();

			if (netplay_is_blacklisted(module_hash) || my_checksum.m_checksums[i] == checksum.m_checksums[i])
				continue;

			resync = true;
			break;
		}

		break;
	}

	if (!checksum_found)
		NETPLAY_LOG("failed to find checksums in history for frame %u", checksum.m_frame_count);

	if (checksum_found && !resync)
		return;

	if (machine().scheduler().can_save())
		store_state();

	send_sync(peer, NETPLAY_SYNC_CHECKSUM_ERROR);
}

bool netplay_manager::socket_connected(const netplay_addr& address)
{
	auto addr = netplay_socket::addr_to_str(address);
	NETPLAY_LOG("received socket connection from %s", addr.c_str());

	if (m_host)
	{
		machine().ui().popup_time(5, "Client connecting");

		// if we're at max capacity then reject the connection
		return m_peers.size() < MAX_PLAYERS;
	}
	else
	{
		machine().ui().popup_time(5, "Connecting to server");

		// add the host to the peers list
		add_peer("server", address);

		// send them the handshake
		BEGIN_PACKET(NETPLAY_HANDSHAKE);
			netplay_handshake handshake;
			handshake.m_name = "client";
			handshake.serialize(writer);
		END_PACKET(address);
	}

	return true;
}

void netplay_manager::socket_disconnected(const netplay_addr& address)
{
	auto addr = netplay_socket::addr_to_str(address);

	for (auto it = m_peers.begin(); it != m_peers.end(); ++it)
	{
		if ((*it)->address() == address)
		{
			machine().ui().popup_time(5, "Peer '%s' has disconnected.", (*it)->name().c_str());
			NETPLAY_LOG("peer %s disconnected", (*it)->name().c_str());
			m_peers.erase(it);
			break;
		}
	}

	m_waiting_for_client = false;
}

void netplay_manager::socket_data(netplay_socket_reader& reader, const netplay_addr& sender)
{
	m_stats.m_packets_received++;

	// read the packet header
	unsigned int flags;
	unsigned int sync_generation;
	netplay_packet_read(reader, flags, sync_generation);

	// discard any packets with a lower sync generation than ours
	// if (sync_generation < m_sync_generation)
	//	return;

	if (m_host)
		handle_host_packet(reader, flags, sender);
	else
		handle_client_packet(reader, flags, sender);
}

// called by save_manager whenever a device creates a memory block for itself
// populates the corresponding netplay sync blocks
void netplay_manager::create_memory_block(const std::string& module_name, const std::string& name, void* data_ptr, size_t size)
{
	netplay_assert(m_initialized);
	netplay_assert(data_ptr != nullptr);
	netplay_assert(size > 0);

	/*if (m_debug)
	{
		NETPLAY_LOG("creating %d memory blocks for %s with size %zu",
			std::max(1, (int)(size / m_max_block_size)), name.c_str(), size);
	}*/

	auto ptr = (unsigned char*)data_ptr;

	while(size > 0)
	{
		auto index = m_memory.size();
		auto block_size = std::min(size, m_max_block_size);
		
		// m_memory stores the active blocks currently in-use by the emulator
		auto active_block = std::make_shared<netplay_memory>(index, module_name, name, ptr, block_size);
		m_memory.push_back(active_block);

		// m_good_state stores the last acknowledged state between peers used for resync
		auto good_block = std::make_shared<netplay_memory>(index, module_name, name, block_size);
		good_block->copy_from(*active_block);
		m_good_state.m_blocks.push_back(good_block);
		
		// m_states is a circular buffer of previous states used for rollback
		for (auto& state : m_states)
		{
			auto sync_block = std::make_shared<netplay_memory>(index, module_name, name, block_size);
			sync_block->copy_from(*active_block);
			state.m_blocks.push_back(sync_block);
		}

		ptr += block_size;
		size -= block_size;
	}

	netplay_assert(size == 0);
}

// called by ioport every update with the new input state
void netplay_manager::send_input_state(netplay_input& input_state)
{
	if ((!m_host && !m_initial_sync) || m_frame_count <= m_input_delay)
		return;

	netplay_assert(m_initialized);
	netplay_assert(!m_peers.empty());

	// send the inputs to all peers
	for(auto& peer : m_peers)
	{
		if (peer->self())
			continue;

		unsigned int flags = NETPLAY_INPUTS;

		if (!m_host && m_has_ping_time)
			flags |= NETPLAY_PONG;
		if (m_host && m_frame_count % m_ping_every == 0)
			flags |= NETPLAY_PING;

		BEGIN_PACKET(flags)
			input_state.serialize(writer);

			if (flags & NETPLAY_PING)
			{
				writer.write(system_time());
			}
			else if (flags & NETPLAY_PONG)
			{
				writer.write(m_last_ping_time);
				m_has_ping_time = false;
			}
		END_PACKET(peer->address());
	}
}

netplay_peer& netplay_manager::add_peer(const std::string& name, const netplay_addr& address, bool self)
{
	std::shared_ptr<netplay_peer> existing_peer = nullptr;
	for (auto it = m_peers.begin(); it != m_peers.end(); ++it)
	{
		auto& peer = *it;
		if (peer->address() == address)
		{
			existing_peer = peer;
			break;
		}
	}

	auto address_s = netplay_socket::addr_to_str(address);
	if (existing_peer == nullptr)
	{
		NETPLAY_LOG("adding new peer '%s' with address '%s'", name.c_str(), address_s.c_str());
		m_peers.push_back(std::make_shared<netplay_peer>(name, address, system_time(), self));
		return *m_peers.back();
	}

	NETPLAY_LOG("peer %s with address %s already exists, assuming reconnected", name.c_str(), address_s.c_str());
	existing_peer->m_name = name;
	existing_peer->m_join_time = system_time();
	return *existing_peer;
}

attotime netplay_manager::system_time() const
{
#ifdef EMSCRIPTEN
	return attotime::from_double(emscripten_get_now() * 0.001);
#endif
}

netplay_peer* netplay_manager::get_peer_by_addr(const netplay_addr& address) const
{
	for(auto& peer : m_peers)
		if (peer->address() == address)
			return peer.get();

	return nullptr;
}

netplay_state* netplay_manager::get_state_for_frame(netplay_frame frame_index)
{
	for (auto& state : m_states)
	{
		if (state.m_frame_count == frame_index)
			return &state;
	}

	return nullptr;
}

void netplay_manager::print_stats()
{
	NETPLAY_LOG("----------------------------");
	NETPLAY_LOG("frame count = %d", m_frame_count);
	NETPLAY_LOG("successful rollbacks = %d", m_stats.m_rollback_success)
	NETPLAY_LOG("failed rollbacks = %d", m_stats.m_rollback_fail);
	NETPLAY_LOG("max latency = %dms", m_stats.m_max_latency);
	NETPLAY_LOG("packets sent = %d", m_stats.m_packets_sent);
	NETPLAY_LOG("packets received = %d", m_stats.m_packets_received);
	NETPLAY_LOG("sync (total bytes) = %d", m_stats.m_sync_total_bytes);
	
	auto& syncs = m_stats.m_full_syncs;

	for (auto i = 0; i < NETPLAY_SYNC_END; i++)
	{
		switch (i)
		{
			case NETPLAY_SYNC_INITIAL:
				NETPLAY_LOG("sync (initial) = %d", syncs[NETPLAY_SYNC_INITIAL]);
				break;
			case NETPLAY_SYNC_ROLLBACK_FAILED:
				NETPLAY_LOG("sync (rollback) = %d", syncs[NETPLAY_SYNC_ROLLBACK_FAILED]);
				break;
			case NETPLAY_SYNC_CHECKSUM_ERROR:
				NETPLAY_LOG("sync (checksum) = %d", syncs[NETPLAY_SYNC_CHECKSUM_ERROR]);
		}
	}

	NETPLAY_LOG("----------------------------\n");
}
