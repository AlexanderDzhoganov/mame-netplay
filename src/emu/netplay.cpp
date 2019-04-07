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

#include "netplay/util.h"
#include "netplay.h"
#include "netplay/packet.h"
#include "netplay/memory.h"
#include "netplay/input_state.h"
#include "netplay/peer.h"
#include "netplay/module_blacklist.h"

#define BEGIN_PACKET(FLAGS)                         \
	netplay_memory_stream stream;                     \
	netplay_socket_writer writer(stream);             \
	netplay_packet_write(writer, FLAGS, m_sync_generation, m_next_packet_id++);

#define END_PACKET(ADDR) do {   \
	m_socket->send(stream, ADDR); \
	m_stats.m_packets_sent++;     \
} while(0);

/* * *   TODO   * * *
 * - posix & win32 sockets
 * - support more than 1 client
 * - input remapping
 */

//-------------------------------------------------
// netplay_manager
//-------------------------------------------------

netplay_manager::netplay_manager(running_machine& machine) : 
	 m_machine(machine),
	 m_initialized(false),
	 m_sync_generation(0),
	 m_next_packet_id(0),
	 m_catching_up(false),
	 m_waiting_for_peer(false),
	 m_waiting_for_inputs(false),
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
	m_checksum_every = 217;                       // checksum every N frames
	m_ping_every = 5;                             // ping every N frames
	m_max_rollback = 4;                           // max rollback of N frames

	for (auto i = 0; i < m_states.capacity(); i++)
		m_states.push_back(netplay_state());
}

bool netplay_manager::initialize()
{
	netplay_assert(!m_initialized);
	NETPLAY_LOG("initializing netplay");

	auto& memory_entries = machine().save().m_entry_list;
	for (auto& entry : memory_entries)
		create_memory_block(entry->m_module, entry->m_name, entry->m_data, entry->m_typecount * entry->m_typesize);

	m_socket = std::make_unique<netplay_socket>(*this);

	add_peer(m_host ? "server" : "client", m_socket->get_self_address(), true);

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
		auto host_string = machine().options().netplay_host();
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

	perform_rollback();
	update_simulation();

	if (m_host)
	{
		recalculate_input_delay();
		update_checksum_history();
		process_checksums();
	}
	else
	{
		send_checksums();
	}
}

void netplay_manager::update_simulation()
{
	perform_rollback();

	if (!m_set_delay.m_processed && m_set_delay.m_frame_count >= m_frame_count)
	{
		m_input_delay = m_set_delay.m_input_delay;
		m_set_delay.m_processed = true;
	}
	
	if (m_waiting_for_peer)
		return;

	if (!peer_inputs_available())
	{
		m_waiting_for_inputs = true;
		return;
	}

	m_waiting_for_inputs = false;

	auto current_frame = m_frame_count;
	while (m_frame_count == current_frame)
		machine().scheduler().timeslice();

	store_state();

	if (m_debug && m_frame_count % 3600 == 0)
	{
		print_stats();
		memset(&m_stats, 0, sizeof(netplay_stats));
	}
}

void netplay_manager::perform_rollback()
{
	if (!m_rollback)
		return;
		
	if (!rollback(m_rollback_frame) && m_host)
	{
		// failing a rollback means we're most likely desynced by a large margin
		// send a full sync to all clients
		NETPLAY_LOG("rollback failed, resyncing..");

		for(auto& peer : m_peers)
		{
			if (peer->self())
				continue;
			
			send_sync(*peer, NETPLAY_SYNC_RESYNC);
		}
	}

	m_rollback = false;
}

void netplay_manager::recalculate_input_delay()
{
	if ((m_frame_count % 120 != 0) || m_peers.size() <= 1 || !m_set_delay.m_processed)
		return;

	auto target_latency = 0.0;

	// find the peer with the highest latency
	for (auto& peer : m_peers)
	{
		if (peer->self())
			continue;
		
		auto avg_latency = peer->average_latency();
		auto highest_latency = peer->highest_latency();

		if (highest_latency > m_stats.m_max_latency)
			m_stats.m_max_latency = (unsigned int)highest_latency;

		// if the highest latency is more than 4/3rds the average latency
		// take the highest latency as a target
		if ((4.0 / 3.0) * avg_latency < highest_latency)
			avg_latency = highest_latency;

		target_latency = std::max(target_latency, avg_latency);
	}

	// adjust the input delay
	auto calculated_delay = (unsigned int)(target_latency / (1000.0 / 60.0));

	auto input_delay = m_input_delay;
	if (calculated_delay < input_delay)
		input_delay--;
	else if (calculated_delay > input_delay)
		input_delay++;

	input_delay = std::max(m_input_delay_min, std::min(input_delay, m_input_delay_max));

	if (m_input_delay == input_delay)
		return;

	m_set_delay.m_input_delay = input_delay;
	m_set_delay.m_frame_count = m_frame_count + 2 * m_input_delay;
	m_set_delay.m_processed = false;

	NETPLAY_LOG("setting input delay to '%d'", input_delay);

	for(auto& peer : m_peers)
	{
		if (peer->self())
			continue;

		BEGIN_PACKET(NETPLAY_SET_DELAY);
			m_set_delay.serialize(writer);
		END_PACKET(peer->address());
	}
}

void netplay_manager::update_checksum_history()
{
	if (m_frame_count % m_checksum_every != 0)
		return;

	auto& state = m_states.newest();

	netplay_checksum* checksum = nullptr;
	for (auto i = 0u; i < m_checksums_history.size(); i++)
	{
		if (m_checksums_history[i].m_frame_count == state.m_frame_count)
		{
			checksum = &m_checksums_history[i];
			break;
		}
	}

	if (checksum == nullptr)
	{
		m_checksums_history.push_back(netplay_checksum());
		checksum = &m_checksums_history.newest();
	}

	checksum->m_frame_count = state.m_frame_count;
	checksum->m_checksums.resize(state.m_blocks.size());

	for (auto i = 0u; i < state.m_blocks.size(); i++)
		checksum->m_checksums[i] = state.m_blocks[i]->checksum();
}

void netplay_manager::process_checksums()
{
	for (auto i = 0; i < m_checksums.size(); i++)
	{
		auto& checksum = m_checksums[i];
		if (checksum.m_processed || checksum.m_frame_count >= m_frame_count)
			continue;

		netplay_assert(m_peers.size() >= 2);
		handle_checksum(checksum, *m_peers[1]);
		checksum.m_processed = true;
	}
}

void netplay_manager::send_checksums()
{
	if ((m_frame_count % m_checksum_every != 0) || !m_initial_sync)
		return;

	auto& state = m_states.newest();
	auto& blocks = state.m_blocks;

	BEGIN_PACKET(NETPLAY_CHECKSUM);
		netplay_checksum checksum;
		checksum.m_frame_count = state.m_frame_count;
		checksum.m_checksums.resize(blocks.size());

		for (auto i = 0; i < blocks.size(); i++)
		{
			if (netplay_is_blacklisted(blocks[i]->module_hash()))
				continue;

			checksum.m_checksums[i] = blocks[i]->checksum();
		}

		checksum.serialize(writer);
	END_PACKET(m_peers[1]->address());
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

	netplay_state* state = nullptr;
	for (auto i = 0; i < m_states.size(); i++)
	{
		if (m_states[i].m_frame_count == m_frame_count)
		{
			state = &m_states[i];
		}
	}

	if (state == nullptr)
	{
		// move to the next buffer in the states list
		m_states.advance(1);
		state = &m_states.newest();
	}

	auto& blocks = state->m_blocks;

	// tell devices to save their memory
	machine().save().dispatch_presave();

	netplay_assert(blocks.size() == m_memory.size());

	// copy over the blocks from active memory to the new state
	for (auto i = 0; i < m_memory.size(); i++)
		blocks[i]->copy_from(*m_memory[i]);

	// record metadata
	state->m_frame_count = m_frame_count;
	return true;
}

// load the latest sync point
// copy over data from sync blocks to active blocks
void netplay_manager::load_state(const netplay_state& state)
{
	netplay_assert(state.m_blocks.size() == m_memory.size());

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

	// store the current time, we'll advance the simulation to it later
	auto start_frame = m_frame_count;

	netplay_state* rollback_state = nullptr;
	netplay_frame state_frame = 0;

	// find the newest state we can rollback to
	for (auto i = 0; i < m_states.size(); i++)
	{
		auto& state = m_states[i];

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
	{
		auto prev_frame = m_frame_count;
		while (m_frame_count == prev_frame)
			machine().scheduler().timeslice();

		store_state();

		if (m_host)
			update_checksum_history();
	}

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
	store_state();

	m_stats.m_syncs++;
	m_sync_generation++;
	m_rollback = false;
	m_waiting_for_peer = true;
	m_set_delay.m_processed = true;
	m_checksums.clear();

	for (auto& peer : m_peers)
		peer->m_last_input_frame = 0;

	auto& state = m_states.newest();
	bool full_sync = reason == NETPLAY_SYNC_INITIAL || reason == NETPLAY_SYNC_RESYNC;

	BEGIN_PACKET(NETPLAY_SYNC);
		netplay_sync sync;
		sync.m_frame_count = state.m_frame_count;
		sync.m_input_delay = m_input_delay;
		sync.serialize(writer);

		for (auto i = 0; i < state.m_blocks.size(); i++)
		{
			auto& block = state.m_blocks[i];
			auto& good_block = m_good_state.m_blocks[i];

			// skip any blocks with the same checksum in the known good state
			// except if we're doing an initial sync or a full resync, then send everything
			bool checksums_match = block->checksum() == good_block->checksum();
			if (!full_sync && checksums_match)
				continue;

			good_block->copy_from(*block);

			// add the block to the packet
			netplay_packet_add_block(writer, *block);
		}

		m_stats.m_sync_total_bytes += writer.stream().cursor();
	END_PACKET(peer.address());

	if (m_debug)
	{
		auto checksum = m_good_state.checksum();
		NETPLAY_LOG("sending sync: full = %d, frame = %d, checksum = %#08x", full_sync, state.m_frame_count, checksum);
	}
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
		m_waiting_for_peer = false;
		return;
	}
	
	auto peer = get_peer_by_addr(sender);
	
	// if we just resynced the client and she hasn't acknowledged yet
	// skip all packets because they contain old data
	if (m_waiting_for_peer || peer == nullptr)
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
		m_set_delay.deserialize(reader);
		m_set_delay.m_processed = false;

		NETPLAY_LOG("setting input delay to '%d'", m_set_delay.m_input_delay);
		return;
	}
}

void netplay_manager::handle_handshake(const netplay_handshake& handshake, const netplay_addr& address)
{
	// add the peer to our list and send the initial sync
	auto& peer = add_peer(handshake.m_name, address);
	peer.m_next_packet_id = 1;

	if (m_debug)
		NETPLAY_LOG("initial sync with %s at %d", peer.name().c_str(), m_frame_count);

	send_sync(peer, NETPLAY_SYNC_INITIAL);
}

void netplay_manager::handle_sync(const netplay_sync& sync, netplay_socket_reader& reader, netplay_peer& peer)
{
	m_stats.m_syncs++;
	m_stats.m_sync_total_bytes += reader.stream().size();

	m_sync_generation++;
	m_input_delay = sync.m_input_delay;
	m_set_delay.m_processed = true;

	for (auto& peer : m_peers)
		peer->m_last_input_frame = 0;

	m_good_state.m_frame_count = sync.m_frame_count;

	// copy the memory blocks over
	netplay_packet_read_blocks(reader, m_good_state.m_blocks);

	if (m_debug)
	{
		auto checksum = m_good_state.checksum();
		NETPLAY_LOG("received sync: frame = %d, size = %lu, checksum = %#08x",
			sync.m_frame_count, reader.stream().size(), checksum);
	}

	// restore the machine state
	load_state(m_good_state);

	// store the new state
	store_state();

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
	peer.m_last_input_frame = effective_frame;

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
	
	auto& state = m_states.newest();
	auto& blocks = state.m_blocks;

	for (auto i = 0; i < m_checksums_history.size(); i++)
	{
		auto& my_checksum = m_checksums_history[i];
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

			NETPLAY_LOG("checksum error in '%s' (%#08x)", blocks[i]->module_name().c_str(), module_hash);

			resync = true;
			break;
		}

		break;
	}

	if (!checksum_found)
	{
		NETPLAY_LOG("failed to find checksums in history for frame %u", checksum.m_frame_count);
		return;
	}

	if (checksum_found && !resync)
		return;

	send_sync(peer, NETPLAY_SYNC_CHECKSUM_ERROR);
}

bool netplay_manager::socket_connected(const netplay_addr& address)
{
	auto addr = netplay_socket::addr_to_str(address);
	NETPLAY_LOG("received socket connection from %s", addr.c_str());

	if (m_host)
	{
		// if we're at max capacity then reject the connection
		return m_peers.size() < MAX_PLAYERS;
	}

	// if we're the client then add the host to the peers list
	add_peer("server", address);

	// and send them the handshake
	BEGIN_PACKET(NETPLAY_HANDSHAKE);
		netplay_handshake handshake;
		handshake.m_name = "client";
		handshake.serialize(writer);
	END_PACKET(address);
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
			NETPLAY_LOG("peer '%s' disconnected", (*it)->name().c_str());
			m_peers.erase(it);
			break;
		}
	}

	m_waiting_for_peer = false;
}

void netplay_manager::socket_data(netplay_socket_reader& reader, const netplay_addr& sender)
{
	m_stats.m_packets_received++;

	// read the packet header
	unsigned int flags;
	unsigned int sync_generation;
	unsigned int packet_id;
	netplay_packet_read(reader, flags, sync_generation, packet_id);

	auto peer = get_peer_by_addr(sender);
	if (peer != nullptr)
	{
		if (packet_id != peer->m_next_packet_id)
		{
			NETPLAY_LOG("(ERROR) packet %d arrived out of order (expected %d)", packet_id, peer->m_next_packet_id);
		}

		peer->m_next_packet_id++;
	}

	// discard any packets with a lower sync generation than ours except handshakes
	if (sync_generation < m_sync_generation && !(flags & NETPLAY_HANDSHAKE))
		return;

	if (m_host)
		handle_host_packet(reader, flags, sender);
	else
		handle_client_packet(reader, flags, sender);
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
	auto address_s = netplay_socket::addr_to_str(address);

	std::shared_ptr<netplay_peer> existing_peer = nullptr;
	for (auto it = m_peers.begin(); it != m_peers.end(); ++it)
	{
		auto& peer = *it;
		if (peer->address() == address)
		{
			NETPLAY_LOG("peer '%s' (address = '%s') has reconnected", name.c_str(), address_s.c_str());
			machine().ui().popup_time(5, "Reconnected to '%s'.", name.c_str());
			existing_peer = peer;
			m_peers.erase(it);
			break;
		}
	}

	if (!existing_peer)
	{
		NETPLAY_LOG("got new peer '%s' (address = '%s')", name.c_str(), address_s.c_str());
	}

	machine().ui().popup_time(5, "Connected to '%s'.", name.c_str());
	m_peers.push_back(std::make_shared<netplay_peer>(name, address, system_time(), self));
	return *m_peers.back();
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

bool netplay_manager::peer_inputs_available() const
{
	for(auto& peer : m_peers)
	{
		if (peer->self())
			continue;

		auto last_input_frame = peer->m_last_input_frame;
		if (last_input_frame == 0)
			continue;

		last_input_frame += m_max_rollback;
		if (last_input_frame <= m_frame_count)
		{
			static netplay_frame last_wait = 0;

			if (last_wait != m_frame_count)
				NETPLAY_LOG("waiting for inputs at %d (last = %d)", m_frame_count, last_input_frame);

			last_wait = m_frame_count;
			return false;
		}
	}

	return true;
}

void netplay_manager::create_memory_block(const std::string& module_name, const std::string& name, void* data_ptr, size_t size)
{
	netplay_assert(data_ptr != nullptr);
	netplay_assert(size > 0);

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
		for (auto i = 0; i < m_states.size(); i++)
		{
			auto sync_block = std::make_shared<netplay_memory>(index, module_name, name, block_size);
			sync_block->copy_from(*active_block);
			m_states[i].m_blocks.push_back(sync_block);
		}

		ptr += block_size;
		size -= block_size;
	}

	netplay_assert(size == 0);
}

void netplay_manager::print_stats() const
{
	std::stringstream ss;

	ss << "----------------------------\n";
	ss << "frame count = " << m_frame_count << "\n";
	ss << "successful rollbacks = " << m_stats.m_rollback_success << "\n";
	ss << "failed rollbacks = " << m_stats.m_rollback_fail << "\n";
	
	if (m_host)
		ss << "max latency = " << m_stats.m_max_latency << "ms\n";
	
	ss << "packets sent = " << m_stats.m_packets_sent << "\n";
	ss << "packets received = " << m_stats.m_packets_received << "\n";
	ss << "sync (total) = " << m_stats.m_syncs << "\n";
	ss << "sync (total bytes) = " << m_stats.m_sync_total_bytes << "\n";
	ss << "----------------------------\n";

	auto s = ss.str();
	NETPLAY_LOG(s.c_str());
}
