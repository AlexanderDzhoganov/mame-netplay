#include <string>
#include <sstream>
#include <unistd.h>
#include <memory>

#include "emu.h"
#include "emuopts.h"
#include "ui/uimain.h"

#include <emscripten.h>

#include "netplay/util.h"
#include "netplay.h"
#include "netplay/packet.h"
#include "netplay/memory.h"
#include "netplay/input_state.h"
#include "netplay/peer.h"
#include "netplay/module_blacklist.h"

#define NETPLAY_MAX_PLAYERS 4

#define PACKET(FLAGS, CODE) do {      \
	netplay_socket_writer packet;       \
	write_packet_header(packet, FLAGS); \
	do { CODE } while(0);               \
	m_stats.m_packets_sent++;           \
} while(0);

#define SEND_TO(ADDR, RELIABLE) m_socket->send(packet.stream(), ADDR, RELIABLE);

#define DATA(DATA) do { DATA.serialize(packet); } while(0);

//-------------------------------------------------
// netplay_manager
//-------------------------------------------------

netplay_manager::netplay_manager(running_machine& machine) : 
	 m_machine(machine),
	 m_initialized(false),
	 m_sync_generation(0),
	 m_frame_count(1),
	 m_catching_up(false),
	 m_waiting_for_inputs(false),
	 m_waiting_for_connection(false)
{
	auto& opts = m_machine.options();
	auto host_address = opts.netplay_host();

	// configuration options
	m_debug = opts.netplay_debug();     // netplay debug printing
	m_name = opts.netplay_name();       // name of this node
	m_host = strlen(host_address) == 0; // is this node the host

	if (!m_host)
		m_host_address = netplay_socket::str_to_addr(host_address);

	m_max_block_size = opts.netplay_block_size(); // 1kb max block size
	m_input_delay_min = 2;                        // minimum input delay
	m_input_delay_max = 20;                       // maximum input delay
	m_input_delay = 5;                            // use N frames of input delay
	m_checksum_every = 31;                        // checksum every N frames
	m_ping_every = 7;                             // ping every N frames
	m_max_rollback = 3;                           // max rollback of N frames

	for (auto i = 0; i < m_states.capacity(); i++)
		m_states.push_back(netplay_state());

	memset(&m_stats, 0, sizeof(netplay_stats));
}

bool netplay_manager::initialize()
{
	netplay_assert(!m_initialized);
	NETPLAY_LOG("initializing netplay");

	auto gameName = machine().system().name;
	EM_ASM_ARGS({
		jsmame_netplay_init($0);
	}, (unsigned int)gameName);

	auto& memory_entries = machine().save().m_entry_list;
	for (auto& entry : memory_entries)
		create_memory_block(entry->m_module, entry->m_name, entry->m_data, entry->m_typecount * entry->m_typesize);

	m_socket = std::make_unique<netplay_socket>(*this);

	if (m_host)
	{
		// this node is the host so it gets to be first in the peers list
		auto& me = add_peer(m_socket->get_self_address(), true);
		me.set_state(NETPLAY_PEER_ONLINE);

		netplay_listen_socket listen_socket;
		if (m_socket->listen(listen_socket) != NETPLAY_NO_ERR)
		{
			NETPLAY_LOG("failed to open listen socket");
			return false;
		}
	}
	else
	{
		if (m_socket->connect(m_host_address) != NETPLAY_NO_ERR)
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

	auto me = my_peer();
	if (me != nullptr)
		me->m_last_system_time = system_time();

	update_simulation();

	if (m_host)
	{
		recalculate_input_delay();
		update_checksum_history();
		process_checksums();
	}
	else
	{
		if (m_waiting_for_connection)
			wait_for_connection();
		else if (m_sync_generation != 0)
			send_checksums();
	}

	if (m_debug && m_frame_count % 3600 == 0)
	{
		print_stats();
		memset(&m_stats, 0, sizeof(netplay_stats));
	}
}

void netplay_manager::update_simulation()
{
	if (!m_host && (m_waiting_for_connection || m_sync_generation == 0))
		return;

	if (waiting_for_peer())
		return;

	if (!m_set_delay.m_processed && m_set_delay.m_frame_count >= m_frame_count)
	{
		m_input_delay = m_set_delay.m_input_delay;
		m_set_delay.m_processed = true;
	}

	m_waiting_for_inputs = true;

	if (!peer_inputs_available())
		return;

	m_waiting_for_inputs = false;

	auto current_frame = m_frame_count;
	while (m_frame_count == current_frame)
		machine().scheduler().timeslice();

	store_state();
}

void netplay_manager::wait_for_connection()
{
	netplay_assert(!m_host);
	netplay_assert(m_waiting_for_connection);

	bool everyone_online = true;
	for (auto& peer : m_peers)
	{
		if (peer->self())
			continue;

		if (peer->state() != NETPLAY_PEER_ONLINE)
		{
			everyone_online = false;
			break;
		}
	}

	if (everyone_online)
	{
		NETPLAY_LOG("connected to all peers, telling server i'm ready");

		PACKET(NETPLAY_READY, {
			netplay_ready ready;
			ready.m_name = m_name;
			DATA(ready);
			SEND_TO(m_peers[0]->address(), true);
		});

		m_waiting_for_connection = false;
	}
}

void netplay_manager::recalculate_input_delay()
{
	netplay_assert(m_host);

	if ((m_frame_count % 20 != 0) || m_peers.size() <= 1 || !m_set_delay.m_processed)
		return;

	float target_latency = 0.0f;

	// find the peer with the highest latency
	for (auto& peer : m_peers)
	{
		if (peer->self())
			continue;
		
		auto avg_latency = peer->latency_estimator().predicted_latency();
		m_stats.m_avg_latency_sum += (unsigned int)avg_latency;
		m_stats.m_avg_latency_n++;

		if (avg_latency > m_stats.m_max_latency)
			m_stats.m_max_latency = avg_latency;

		target_latency = std::max(target_latency, avg_latency);
	}

	// adjust the input delay
	auto input_delay = (unsigned int)(target_latency / (1000.0f / 60.0f)) + 1;
	input_delay = std::max(m_input_delay_min, std::min(input_delay, m_input_delay_max));

	if (m_input_delay == input_delay)
		return;

	m_set_delay.m_input_delay = input_delay;
	m_set_delay.m_frame_count = m_frame_count + input_delay;
	m_set_delay.m_processed = false;

	NETPLAY_LOG("setting input delay to '%d'", input_delay);

	netplay_socket_writer packet;
	write_packet_header(packet, NETPLAY_SET_DELAY);
	m_set_delay.serialize(packet);
	m_socket->broadcast(packet.stream(), true);
}

void netplay_manager::update_checksum_history()
{
	netplay_assert(m_host);

	if (m_frame_count % m_checksum_every != 0)
		return;

	auto& state = m_states.newest();

	netplay_checksum* checksum = nullptr;
	for (auto& stored_checksum : m_checksum_history)
	{
		if (stored_checksum.m_frame_count != state.m_frame_count)
			continue;

		checksum = &stored_checksum;
		break;
	}

	if (checksum == nullptr)
	{
		m_checksum_history.push_back(netplay_checksum());
		checksum = &m_checksum_history.newest();
	}

	checksum->m_frame_count = state.m_frame_count;
	checksum->m_checksums.resize(state.m_blocks.size());

	for (auto i = 0u; i < state.m_blocks.size(); i++)
		checksum->m_checksums[i] = state.m_blocks[i]->checksum();
}

void netplay_manager::process_checksums()
{
	netplay_assert(m_host);

	for (auto& checksum : m_checksums)
	{
		if (checksum.m_processed || checksum.m_frame_count >= m_frame_count)
			continue;

		netplay_assert(m_peers.size() >= 2);

		auto peer = get_peer_by_addr(checksum.m_peer_address);
		if (peer != nullptr)
			handle_checksum(checksum, *peer);
		checksum.m_processed = true;
	}
}

void netplay_manager::send_checksums()
{
	netplay_assert(!m_host);

	if ((m_frame_count % m_checksum_every != 0) || m_sync_generation == 0)
		return;

	auto& state = m_states.newest();
	auto& blocks = state.m_blocks;

	PACKET(NETPLAY_CHECKSUM, {
		netplay_checksum checksum;
		checksum.m_frame_count = state.m_frame_count;
		checksum.m_checksums.resize(blocks.size());

		for (auto i = 0; i < blocks.size(); i++)
		{
			if (netplay_is_blacklisted(blocks[i]->module_hash()))
				continue;

			checksum.m_checksums[i] = blocks[i]->checksum();
		}

		DATA(checksum);
		SEND_TO(m_peers[0]->address(), false);
	});
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
	for (auto& stored_state : m_states)
	{
		if (stored_state.m_frame_count != m_frame_count)
			continue;

		state = &stored_state;
		break;
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
	return true;
}

// syncs all clients
// this is slow and expensive and is used as a last resort in case of a desync
// also used to initialize newly connected peers
void netplay_manager::send_sync(bool full_sync)
{
	netplay_assert(m_host);

	// first store the simulation state
	store_state();

	// increment the sync generation and record stats
	m_sync_generation++;
	m_stats.m_syncs++;

	// remove any 'set delay' command we had buffered
	m_set_delay.m_processed = true;

	// clear all pending checksums
	m_checksums.clear();

	auto& state = m_states.newest();

	netplay_socket_writer packet;
	write_packet_header(packet, NETPLAY_SYNC); 

	netplay_sync sync;
	sync.m_frame_count = state.m_frame_count;
	sync.m_input_delay = m_input_delay;
	sync.serialize(packet);

	for (auto i = 0; i < state.m_blocks.size(); i++)
	{
		auto& block = state.m_blocks[i];
		auto& good_block = m_good_state.m_blocks[i];

		// skip any blocks with the same checksum in the known good state
		// except if we're doing an initial sync or a full resync, then send everything
		if (!full_sync && block->checksum() == good_block->checksum())
			continue;

		good_block->copy_from(*block);

		// add the block to the packet
		netplay_packet_add_block(packet, *block);
	}

	m_socket->broadcast(packet.stream(), true);
	m_stats.m_sync_total_bytes += packet.stream().cursor();
	m_stats.m_packets_sent += m_peers.size() - 1;
	
	for (auto& peer : m_peers)
	{
		if (peer->self())
			continue;

		peer->set_state(NETPLAY_PEER_SYNCING);
		peer->m_last_input_frame = 0;
	}

	if (m_debug)
	{
		auto checksum = m_good_state.checksum();
		NETPLAY_LOG("sending sync: full = %d, frame = %d, checksum = %#08x", full_sync, state.m_frame_count, checksum);
	}
}

void netplay_manager::handle_host_packet(netplay_socket_reader& reader, unsigned char flags, netplay_peer& peer)
{
	netplay_assert(m_host);

	if (flags & NETPLAY_READY)
	{
		netplay_ready ready;
		ready.deserialize(reader);
		handle_ready(ready, peer);
	}
	else if (flags & NETPLAY_SYNC)
	{
		if (peer.state() == NETPLAY_PEER_SYNCING)
			peer.set_state(NETPLAY_PEER_ONLINE);
	}
	else if (flags & NETPLAY_INPUTS)
	{
		auto& input = peer.get_next_input_buffer();
		input.deserialize(reader);
		handle_inputs(input, peer);
	}
	else if (flags & NETPLAY_CHECKSUM)
	{
		netplay_checksum checksum;
		checksum.deserialize(reader);
		checksum.m_peer_address = peer.address();

		if (checksum.m_frame_count >= m_frame_count)
			m_checksums.push_back(checksum);
		else
			handle_checksum(checksum, peer);
	}
}

void netplay_manager::handle_client_packet(netplay_socket_reader& reader, unsigned char flags, netplay_peer& peer)
{
	netplay_assert(!m_host);

	if (flags & NETPLAY_HANDSHAKE)
	{
		netplay_handshake handshake;
		handshake.deserialize(reader);
		handle_handshake(handshake, peer);
	}
	else if (flags & NETPLAY_SYNC)
	{
		netplay_sync sync;
		sync.deserialize(reader);
		handle_sync(sync, reader, peer);
	}
	else if (flags & NETPLAY_INPUTS)
	{
		auto& input = peer.get_next_input_buffer();
		input.deserialize(reader);
		handle_inputs(input, peer);
	}
	else if(flags & NETPLAY_SET_DELAY)
	{
		m_set_delay.deserialize(reader);
		m_set_delay.m_processed = false;

		NETPLAY_LOG("setting input delay to '%d'", m_set_delay.m_input_delay);
	}
}

void netplay_manager::handle_ready(const netplay_ready& ready, netplay_peer& peer)
{
	netplay_assert(m_host);

	// a client is ready, resync everyone
	send_sync(true);
}

void netplay_manager::handle_handshake(const netplay_handshake& handshake, netplay_peer& peer)
{
	netplay_assert(!m_host);

	NETPLAY_LOG("received handshake");

	m_sync_generation = handshake.m_sync_generation;
	m_waiting_for_connection = true;

	// add all the peers the host sent us and try to connect to each
	for (auto& address : handshake.m_peers)
	{
		auto addr = netplay_socket::addr_to_str(address);
		if(m_socket->connect(address) != NETPLAY_NO_ERR)
		{
			NETPLAY_LOG("failed to connect to peer '%s'", addr.c_str());
			return;
		}

		add_peer(address);
		NETPLAY_LOG("got peer '%s'", addr.c_str());
	}
}

void netplay_manager::handle_sync(const netplay_sync& sync, netplay_socket_reader& reader, netplay_peer& peer)
{
	netplay_assert(!m_host);

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
	PACKET(NETPLAY_SYNC, {
		SEND_TO(peer.address(), true);
	});
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
	auto predicted_input = peer.predicted_inputs_for(effective_frame);

	// test if both the actual and predicted inputs are the same
	// if the predicted inputs differ then we do a rollback to just before the input time
	if (predicted_input != nullptr && *predicted_input == input_state)
		return;
	
	rollback(effective_frame);
}

void netplay_manager::handle_checksum(const netplay_checksum& checksum, netplay_peer& peer)
{
	netplay_assert(m_host);

	bool resync = false;
	bool checksum_found = false;
	
	auto& state = m_states.newest();
	auto& blocks = state.m_blocks;

	for (auto& my_checksum : m_checksum_history)
	{
		if (my_checksum.m_frame_count != checksum.m_frame_count)
			continue;

		checksum_found = true;
		netplay_assert(blocks.size() == checksum.m_checksums.size());
		netplay_assert(blocks.size() == my_checksum.m_checksums.size());

		for (auto i = 0; i < blocks.size(); i++)
		{
			auto module_hash = blocks[i]->module_hash();
			bool match = my_checksum.m_checksums[i] == checksum.m_checksums[i];

			if (netplay_is_blacklisted(module_hash) || match)
				continue;

			NETPLAY_LOG("checksum error in '%s' (%#08x)",
				blocks[i]->module_name().c_str(), module_hash);

			resync = true;
			break;
		}

		break;
	}

	if (checksum_found && !resync)
		return;

	send_sync(false);
}

bool netplay_manager::socket_connected(const netplay_addr& address)
{
	auto addr = netplay_socket::addr_to_str(address);
	NETPLAY_LOG("established connection with '%s'", addr.c_str());

	if (m_host)
		return host_socket_connected(address);
	else
		return client_socket_connected(address);
}

bool netplay_manager::host_socket_connected(const netplay_addr& address)
{
	netplay_assert(m_host);

	// if we already have this peer delete them from the peers list
	for (auto it = m_peers.begin(); it != m_peers.end(); ++it)
	{
		if ((*it)->address() == address)
		{
			m_peers.erase(it);
			break;
		}
	}

	// if we're at max capacity then reject the connection
	if (m_peers.size() >= NETPLAY_MAX_PLAYERS)
		return false;

	// add the peer to the peers list and set its state to not ready
	auto& peer = add_peer(address);
	peer.set_state(NETPLAY_PEER_NOT_READY);

	auto addr = netplay_socket::addr_to_str(address);
	NETPLAY_LOG("sending handshake to '%s'", addr.c_str());

	// send the handshake
	PACKET(NETPLAY_HANDSHAKE, {
		netplay_handshake handshake;
		handshake.m_name = m_name;
		handshake.m_sync_generation = m_sync_generation;

		for (auto& peer : m_peers)
		{
			if (peer->self() || peer->address() == address)
				continue;

			handshake.m_peers.push_back(peer->address());
		}

		DATA(handshake);
		SEND_TO(address, true);
	});
	return true;
}

bool netplay_manager::client_socket_connected(const netplay_addr& address)
{
	netplay_assert(!m_host);

	if (address == m_host_address)
	{
		// if we connected to the host then add them to our peers list
		auto& host = add_peer(address);
		host.set_state(NETPLAY_PEER_ONLINE);

		// then add ourselves
		auto& me = add_peer(m_socket->get_self_address(), true);
		me.set_state(NETPLAY_PEER_ONLINE);
		return true;
	}

	// check if we know of this peer
	auto connected_peer = get_peer_by_addr(address);
	if (connected_peer == nullptr)
	{
		// if we don't know who this is then it's a peer that just connected
		auto& peer = add_peer(address);
		peer.set_state(NETPLAY_PEER_ONLINE);
	}
	else
	{
		connected_peer->set_state(NETPLAY_PEER_ONLINE);
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
			NETPLAY_LOG("peer '%s' disconnected", (*it)->name().c_str());
			m_peers.erase(it);
			break;
		}
	}
}

void netplay_manager::socket_data(netplay_socket_reader& reader, const netplay_addr& sender)
{
	auto peer = get_peer_by_addr(sender);
	if (peer == nullptr)
		return;

	// read the packet header
	unsigned char flags;
	if (!read_packet_header(reader, flags, *peer))
		return;

	if (m_host)
		handle_host_packet(reader, flags, *peer);
	else
		handle_client_packet(reader, flags, *peer);

	// record stats
	m_stats.m_packets_received++;
}

// called by ioport every update with the new input state
void netplay_manager::send_input_state(netplay_input& input_state)
{
	netplay_assert(m_initialized);

	// don't send any inputs if we're a client and we still haven't got our first sync
	if (!m_host && m_sync_generation == 0)
		return;

	netplay_socket_writer packet;
	write_packet_header(packet, NETPLAY_INPUTS);
	input_state.serialize(packet);

	m_socket->broadcast(packet.stream(), false);
	m_stats.m_packets_sent += m_peers.size() - 1;
}

netplay_peer& netplay_manager::add_peer(const netplay_addr& address, bool self)
{
	auto addr = netplay_socket::addr_to_str(address);

	std::shared_ptr<netplay_peer> existing_peer = nullptr;
	for (auto it = m_peers.begin(); it != m_peers.end(); ++it)
	{
		auto& peer = *it;
		if (peer->address() == address)
		{
			NETPLAY_LOG("peer '%s' has reconnected", addr.c_str());
			existing_peer = peer;
			m_peers.erase(it);
			break;
		}
	}

	if (!existing_peer && !self)
		NETPLAY_LOG("got new peer '%s'", addr.c_str());

	if (!self)
		machine().ui().popup_time(5, "Connected to '%s'.", addr.c_str());

	m_peers.push_back(std::make_shared<netplay_peer>(address, system_time(), self));
	return *m_peers.back();
}

attotime netplay_manager::system_time() const
{
#ifdef EMSCRIPTEN
	return attotime::from_double(emscripten_get_now() * 0.001);
#else
	return attotime::from_double((double)osd_ticks() / (double)osd_ticks_per_second());
#endif
}

netplay_peer* netplay_manager::my_peer() const
{
	for (auto& peer : m_peers)
		if (peer->self())
			return peer.get();

	return nullptr;
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
				NETPLAY_LOG("waiting for inputs from %s at %d (last = %d)",
					peer->name().c_str(), m_frame_count, last_input_frame);

			last_wait = m_frame_count;
			return false;
		}
	}

	return true;
}

bool netplay_manager::waiting_for_peer() const
{
	for (auto& peer : m_peers)
	{
		if (peer->self())
			continue;

		if (peer->state() != NETPLAY_PEER_ONLINE)
			return true;
	}

	return false;
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

void netplay_manager::write_packet_header(netplay_socket_writer& writer, unsigned char flags)
{
	writer.header('P', 'A', 'K', 'T');
	writer.write(m_sync_generation);
	writer.write(flags);

	writer.write(system_time());

	writer.write((unsigned char)std::max((int)m_peers.size() - 1, 0));
	for(auto& peer : m_peers)
	{
		if (peer->self())
			continue;

		peer->address().serialize(writer);
		writer.write(peer->m_last_system_time);
	}
}

bool netplay_manager::read_packet_header(netplay_socket_reader& reader, unsigned char& flags, netplay_peer& sender)
{
	reader.header('P', 'A', 'K', 'T');

	unsigned int sync_generation;
	reader.read(sync_generation);
	if (sync_generation < m_sync_generation)
		return false;

	reader.read(flags);
	reader.read(sender.m_last_system_time);

	unsigned char num_peers;
	reader.read(num_peers);
	
	bool skip = false;

	for (auto i = 0; i < num_peers; i++)
	{
		netplay_addr address;
		address.deserialize(reader);
		attotime last_system_time;
		reader.read(last_system_time);

		if (skip)
			continue;

		auto peer = get_peer_by_addr(address);
		if (peer == nullptr)
		{
			// this is my timestamp, calculate the latency to the sender
			auto latency_ms = (system_time() - last_system_time).as_double() * 1000.0;

			// add the round-trip latency to this peer's stats
			sender.latency_estimator().add_sample(latency_ms);
			skip = true;
		}
	}

	return true;
}

void netplay_manager::print_stats() const
{
	std::stringstream ss;

	ss << "----------------------------\n";
	ss << "frame count = " << m_frame_count << "\n";
	ss << "successful rollbacks = " << m_stats.m_rollback_success << "\n";
	ss << "failed rollbacks = " << m_stats.m_rollback_fail << "\n";
	
	if (m_host)
	{
		auto avg_latency = m_stats.m_avg_latency_sum / m_stats.m_avg_latency_n;
		ss << "avg latency = " << avg_latency << "ms\n";
		ss << "max latency = " << m_stats.m_max_latency << "ms\n";
	}
	
	ss << "packets sent = " << m_stats.m_packets_sent << "\n";
	ss << "packets received = " << m_stats.m_packets_received << "\n";
	ss << "sync (total) = " << m_stats.m_syncs << "\n";
	ss << "sync (total bytes) = " << m_stats.m_sync_total_bytes << "\n";
	ss << "----------------------------\n";

	auto s = ss.str();
	NETPLAY_LOG(s.c_str());
}
