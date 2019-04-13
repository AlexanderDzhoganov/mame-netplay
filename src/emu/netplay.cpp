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

#define NETPLAY_MAX_PLAYERS 4
#define NETPLAY_INPUT_DELAY_MAX 12

#define PACKET(FLAGS, CODE) do {      \
	netplay_socket_writer packet;       \
	write_packet_header(packet, FLAGS); \
	do { CODE } while(0);               \
	m_stats.m_packets_sent++;           \
} while(0)

#define SEND_TO(ADDR, RELIABLE) m_socket->send(packet.stream(), ADDR, RELIABLE)
#define BROADCAST(RELIABLE) m_socket->broadcast(packet.stream(), RELIABLE)

#define DATA(DATA) do { DATA.serialize(packet); } while(0)

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
	 m_waiting_for_connection(false),
	 m_input_delay_backoff(0),
	 m_next_peerid(1)
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
	m_input_delay = 5;                            // use N frames of input delay
	m_max_rollback = 5;                           // max rollback of N frames
	m_input_redundancy = 10;                      // how many frames of inputs to send per packet

	for (auto i = 0; i < 6; i++)
		m_states.emplace_back();

	memset(&m_stats, 0, sizeof(netplay_stats));
}

bool netplay_manager::initialize()
{
	netplay_assert(!m_initialized);
	NETPLAY_LOG("initializing netplay");

	for (auto& entry : machine().save().m_entry_list)
		create_memory_block(entry->m_module, entry->m_name, entry->m_data, entry->m_typecount * entry->m_typesize);

	EM_ASM_ARGS({ jsmame_netplay_init($0); }, (unsigned int)machine().system().name);

	m_socket = std::make_unique<netplay_socket>(*this);

	if (m_host)
	{
		// this node is the host so it gets to be first in the peers list
		auto& me = add_peer(0, m_socket->get_self_address(), true);
		me.set_state(NETPLAY_PEER_ONLINE);
		me.m_name = m_name;

		netplay_listen_socket listen_socket;
		if (m_socket->listen(listen_socket) != NETPLAY_NO_ERR)
			return false;
	}
	else if (m_socket->connect(m_host_address) != NETPLAY_NO_ERR)
	{
		return false;
	}

	m_initialized = true;
	return true;
}

void netplay_manager::update()
{
	netplay_assert(m_initialized);

	NETPLAY_LOG("begin update");

	if (wait_for_connection())
		return;

	auto& next_delay = m_next_input_delay;
	if (!next_delay.m_processed && next_delay.m_effective_frame == m_frame_count)
	{
		static netplay_frame frames = 0;

		if (!can_save())
		{
			if (frames++ >= 240)
			{
				NETPLAY_LOG("timed out while waiting for inputs, resyncing...");
				send_sync(false);
				return;
			}

			NETPLAY_LOG("have new input delay but not clean yet, waiting...");
			return;
		}

		frames = 0;

		set_input_delay(next_delay.m_input_delay);
		next_delay.m_processed = true;
	}

	static netplay_frame frames_waited = 0;
	if (can_save())
	{
		store_state();
	}
	else
	{
		auto newest_state = get_newest_state();
		netplay_assert(newest_state != nullptr);

		if (m_host && frames_waited++ >= 240)
		{
			NETPLAY_LOG("timed out while waiting for inputs, resyncing");
			send_sync(false);
			return;
		}

		if (newest_state->m_frame_count + m_max_rollback <= m_frame_count)
		{
			NETPLAY_LOG("waiting for inputs...");
			return;
		}
	}

	frames_waited = 0;

	auto current_frame = m_frame_count;
	while (m_frame_count == current_frame)
		machine().scheduler().timeslice();

	if (m_host)
	{
		recalculate_input_delay();
		verify_checksums();
	}
	else
	{
		send_checksums();
	}

	// every N frames garbage collect the input buffers
	auto gc_every = 60;
	if (m_frame_count % gc_every && m_frame_count > gc_every)
		for (auto& peer : m_peers)
			peer->gc_buffers(m_frame_count - gc_every);

	NETPLAY_LOG("end update");

	if (m_debug)
	{
		static netplay_frame last_stats_frame = 0;
		if (m_frame_count >= last_stats_frame + 2000)
		{
			print_stats();
			memset(&m_stats, 0, sizeof(netplay_stats));
			last_stats_frame = m_frame_count;
		}
	}
}

unsigned int netplay_manager::calculate_input_delay()
{
	float target_latency = 0.0f;

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

	auto input_delay = (int)(target_latency / (1000.0f / 60.0f));
	return std::min(std::max(input_delay, 0), NETPLAY_INPUT_DELAY_MAX);
}

void netplay_manager::recalculate_input_delay()
{
	netplay_assert(m_host);

	if (!m_next_input_delay.m_processed)
		return;

	if (m_input_delay_backoff > 0)
		m_input_delay_backoff--;

	if (m_input_delay_backoff != 0 || m_peers.size() <= 1)
		return;

	auto input_delay = calculate_input_delay();
	if (m_input_delay == input_delay)
		return;

	NETPLAY_LOG("setting next input delay to '%d'", input_delay);

	m_input_delay_backoff = 90;

	m_next_input_delay.m_processed = false;
	m_next_input_delay.m_input_delay = input_delay;
	m_next_input_delay.m_effective_frame = m_frame_count + 60;

	PACKET(NETPLAY_DELAY, {
		DATA(m_next_input_delay);
		BROADCAST(true);
	});
}

// create the next sync point
// copy over data from active blocks to sync blocks
netplay_state* netplay_manager::store_state()
{
	if (!machine().scheduler().can_save())
	{
		NETPLAY_LOG("(WARNING) cannot store_state() because scheduler().can_save() == false");
		return nullptr;
	}

	NETPLAY_LOG("storing state at %d", m_frame_count);

	netplay_state* state = get_state_for(m_frame_count);
	if (state == nullptr)
		state = get_oldest_state();

	auto& blocks = state->m_blocks;

	// tell devices to save their memory
	machine().save().dispatch_presave();

	netplay_assert(blocks.size() == m_memory.size());

	// copy over the blocks from active memory to the new state
	for (auto i = 0; i < m_memory.size(); i++)
		blocks[i]->copy_from(*m_memory[i]);

	// record metadata
	state->m_frame_count = m_frame_count;

	// this is necessary or we get desyncs
	machine().save().dispatch_postload();

	if (!m_host)
	{
		m_pending_checksums[state->m_frame_count] = state->checksum();
	}
	else
	{
		auto me = my_peer();
		netplay_assert(me != nullptr);
		me->m_checksums[state->m_frame_count] = state->checksum();
	}

	return state;
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

	// find the newest state we can rollback to
	for (auto& state : m_states)
	{
		if (state.m_frame_count >= before_frame)
			continue;

		if (rollback_state == nullptr || state.m_frame_count > rollback_state->m_frame_count)
			rollback_state = &state;
	}

	if (rollback_state == nullptr)
	{
		// the given time is too far in the past and there is no state to rollback to
		// bail out and let the caller figure it out
		NETPLAY_LOG("rollback from %d to %d failed", m_frame_count, before_frame);
		m_stats.m_rollback_fail++;
		return false;
	}

	NETPLAY_LOG("rollback from %d to %d (before = %d)",
		start_frame, rollback_state->m_frame_count, before_frame)

	// restore the machine to the chosen state
	load_state(*rollback_state);

	// let the emulator know we need to catch up
	// this should disable sound and video updates during the simulation loop below
	m_catching_up = true;

	// run the emulator loop, this will replay any new inputs that we have received since
	while (m_frame_count != start_frame)
		machine().scheduler().timeslice();

	m_catching_up = false;

	for (auto& peer : m_peers)
		peer->m_checksums.clear();

	m_stats.m_rollback_success++;
	return true;
}

// syncs all clients
// this is slow and expensive and is used as a last resort in case of a desync
// also used to initialize newly connected peers
void netplay_manager::send_sync(bool full_sync)
{
	netplay_assert(m_host);

	// increment the sync generation and record stats
	m_sync_generation++;
	m_stats.m_syncs++;
	m_next_input_delay.m_processed = true;
	m_input_delay_backoff = 31;
	m_input_delay = calculate_input_delay();

	auto state = store_state();
	netplay_assert(state != nullptr);

	for (auto& peer : m_peers)
	{
		if (peer->self())
			peer->m_inputs.clear();

		peer->m_next_inputs_at = state->m_frame_count + m_input_delay + 1;
		peer->m_checksums.clear();
		peer->m_predicted_inputs.clear();
	}

	netplay_socket_writer packet;
	write_packet_header(packet, NETPLAY_SYNC); 

	netplay_sync sync;
	sync.m_frame_count = state->m_frame_count;
	sync.m_input_delay = m_input_delay;
	sync.serialize(packet);

	m_good_state.m_frame_count = state->m_frame_count;

	for (auto i = 0; i < state->m_blocks.size(); i++)
	{
		auto& block = state->m_blocks[i];
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

	if (m_debug)
	{
		auto checksum = m_good_state.checksum();
		auto size = packet.stream().cursor();
		NETPLAY_LOG("sending %ssync: frame = %d, checksum = %#08x, size = %lu, next_inputs = %d",
			full_sync ? "full " : "", state->m_frame_count, checksum, size, state->m_frame_count + m_input_delay + 1);
	}
}

void netplay_manager::handle_host_packet(netplay_socket_reader& reader, unsigned char flags, netplay_peer& peer)
{
	netplay_assert(m_host);

	if (flags & NETPLAY_READY)
	{
		netplay_ready ready;
		ready.deserialize(reader);
		peer.m_name = ready.m_name;
		send_sync(true);
	}
	else if (flags & NETPLAY_INPUTS)
	{
		handle_inputs_packet(reader, peer);
	}
	else if (flags & NETPLAY_CHECKSUM)
	{
		netplay_checksum checksum;
		checksum.deserialize(reader);
		peer.m_checksums[checksum.m_frame_count] = checksum.m_checksum;
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
		handle_inputs_packet(reader, peer);
	}
	else if (flags & NETPLAY_DELAY)
	{
		m_next_input_delay.deserialize(reader);
		m_next_input_delay.m_processed = false;
	}
}

void netplay_manager::handle_handshake(const netplay_handshake& handshake, netplay_peer& peer)
{
	netplay_assert(!m_host);

	NETPLAY_LOG("received handshake");

	m_sync_generation = handshake.m_sync_generation;
	m_waiting_for_connection = true;

	peer.m_name = handshake.m_name;

	auto me = my_peer();
	netplay_assert(me != nullptr);
	me->m_peerid = handshake.m_peerid;

	// add all the peers the host sent us and try to connect to each
	for (auto& pair : handshake.m_peers)
	{
		auto addr = netplay_socket::addr_to_str(pair.second);
		if(m_socket->connect(pair.second) != NETPLAY_NO_ERR)
		{
			NETPLAY_LOG("failed to connect to peer '%s'", addr.c_str());
			return;
		}

		add_peer(pair.first, pair.second);
		NETPLAY_LOG("got peer '%s' (id = %d)", addr.c_str(), pair.first);
	}
}

void netplay_manager::handle_sync(const netplay_sync& sync, netplay_socket_reader& reader, netplay_peer& peer)
{
	netplay_assert(!m_host);

	m_sync_generation++;
	m_good_state.m_frame_count = sync.m_frame_count;

	m_input_delay = sync.m_input_delay;
	m_next_input_delay.m_processed = true;

	for (auto& peer : m_peers)
	{
		if (peer->self())
			peer->m_inputs.clear();

		peer->m_next_inputs_at = sync.m_frame_count + m_input_delay + 1;
		peer->m_predicted_inputs.clear();
	}

	// copy the memory blocks over
	netplay_packet_read_blocks(reader, m_good_state.m_blocks);

	// restore the machine state
	load_state(m_good_state);

	// store the new state
	store_state();

	m_stats.m_syncs++;
	m_stats.m_sync_total_bytes += reader.stream().size();

	if (m_debug)
	{
		auto checksum = m_good_state.checksum();
		NETPLAY_LOG("received sync: frame = %d, size = %lu, checksum = %#08x",
			sync.m_frame_count, reader.stream().size(), checksum);
	}
}

void netplay_manager::handle_inputs_packet(netplay_socket_reader& reader, netplay_peer& peer)
{
	auto rollback_frame = m_frame_count + 1;
	auto& inputs = peer.m_inputs;

	netplay_input input;
	for (auto i = 0u; i < m_input_redundancy; i++)
	{
		input.deserialize(reader);
		if (input.m_frame_index == 0)
			continue;

		auto input_frame = input.m_frame_index;

		auto it = inputs.find(input_frame);
		if (it != inputs.end())
			continue;

		NETPLAY_LOG("received inputs for %d", input_frame);

		inputs[input_frame] = input;

		// if the input is in the future it means we're falling behind, record it and move on
		if (input_frame > m_frame_count)
			continue;

		// get any predicted input we used in place of this one
		auto predicted_input = peer.predicted_inputs_for(input_frame);

		// test if both the actual and predicted inputs are the same
		// if the predicted inputs differ then we do a rollback to just before the input time
		bool inputs_match = predicted_input != nullptr && *predicted_input == input;
		peer.m_predicted_inputs.erase(input_frame);

		if (inputs_match)
		{
			NETPLAY_LOG("prediction for %d is OK", input.m_frame_index);
			continue;
		}

		NETPLAY_LOG("prediction for %d is incorrect", input.m_frame_index);

		if (input_frame < rollback_frame)
			rollback_frame = input_frame;
	}

	if (m_max_rollback != 0 && rollback_frame <= m_frame_count)
		rollback(rollback_frame);
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
	auto& peer = add_peer(m_next_peerid++, address);
	peer.set_state(NETPLAY_PEER_NOT_READY);

	auto addr = netplay_socket::addr_to_str(address);
	NETPLAY_LOG("sending handshake to '%s'", addr.c_str());

	netplay_handshake handshake;
	handshake.m_name = m_name;
	handshake.m_peerid = peer.m_peerid;
	handshake.m_sync_generation = m_sync_generation;

	for (auto& peer : m_peers)
	{
		if (peer->self() || peer->m_address == address)
			continue;

		handshake.m_peers.push_back(std::make_pair(peer->m_peerid, peer->m_address));
	}

	// send the handshake
	PACKET(NETPLAY_HANDSHAKE, {
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
		auto& host = add_peer(0, address);
		host.set_state(NETPLAY_PEER_ONLINE);

		// then add ourselves, we take a temporary id of 255 until we learn the actual one
		auto& me = add_peer(255, m_socket->get_self_address(), true);
		me.m_name = m_name;
		me.set_state(NETPLAY_PEER_ONLINE);
		return true;
	}

	// check if we know of this peer
	auto connected_peer = get_peer_by_addr(address);
	if (connected_peer == nullptr)
	{
		// if we don't know who this is then it's a peer that just connected
		// they take a temporary id of 255 until we get the actual one
		auto& peer = add_peer(255, address);
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
void netplay_manager::send_input_state(netplay_frame frame_index)
{
	netplay_assert(m_initialized);

	// don't send inputs if we're a client and haven't gotten the initial sync
	if (!m_host && m_sync_generation == 0)
		return;

	NETPLAY_LOG("sending inputs for %d", frame_index);

	netplay_socket_writer packet;
	write_packet_header(packet, NETPLAY_INPUTS, true);

	netplay_peer* me = my_peer();
	netplay_assert(me != nullptr);
	auto& inputs = me->inputs();

	netplay_input dummy;
	dummy.m_frame_index = 0;

	// send the last N input states for redundancy
	for (auto i = 0; i < m_input_redundancy; i++)
	{
		if (i > frame_index)
		{
			dummy.serialize(packet);
			continue;
		}

		auto it = inputs.find(frame_index - i);
		if (it == inputs.end())
		{
			dummy.serialize(packet);
			continue;
		}

		it->second.serialize(packet);
	}

	m_socket->broadcast(packet.stream(), false);
	m_stats.m_packets_sent += m_peers.size() - 1;
}

netplay_peer& netplay_manager::add_peer(unsigned char peerid, const netplay_addr& address, bool self)
{
	auto addr = netplay_socket::addr_to_str(address);

	std::shared_ptr<netplay_peer> existing_peer = nullptr;
	for (auto it = m_peers.begin(); it != m_peers.end(); ++it)
	{
		auto& peer = *it;
		if (peer->m_address == address)
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

	m_peers.push_back(std::make_shared<netplay_peer>(peerid, address, system_time(), self));
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
		if (peer->m_address == address)
			return peer.get();

	return nullptr;
}

netplay_peer* netplay_manager::get_peer_by_peerid(unsigned char peerid) const
{
	for(auto& peer : m_peers)
		if (peer->m_peerid == peerid)
			return peer.get();

	return nullptr;
}

bool netplay_manager::wait_for_connection()
{
	if (!m_waiting_for_connection)
		return false;

	bool everyone_online = true;
	for (auto& peer : m_peers)
	{
		if (peer->self() || peer->state() == NETPLAY_PEER_ONLINE)
			continue;

		everyone_online = false;
		break;
	}

	if (!everyone_online)
		return true;

	NETPLAY_LOG("connected to all peers, telling server i'm ready");

	netplay_ready ready;
	ready.m_name = m_name;

	PACKET(NETPLAY_READY, {
		DATA(ready);
		SEND_TO(m_peers[0]->address(), true);
	});

	m_waiting_for_connection = false;
	return false;
}

void netplay_manager::set_input_delay(unsigned int input_delay)
{
	if (input_delay > m_input_delay)
		for (auto& peer : m_peers)
			peer->m_next_inputs_at = m_frame_count + input_delay + 1;

	m_input_delay = input_delay;
	NETPLAY_LOG("setting input delay to %d at %d", m_input_delay, m_frame_count);
}

void netplay_manager::send_checksums()
{
	auto& checksums = m_pending_checksums;

	for (auto it = checksums.begin(); it != checksums.end(); ++it)
	{
		if (it->first >= m_frame_count - 21)
			continue;

		netplay_checksum checksum;
		checksum.m_frame_count = it->first;
		checksum.m_checksum = it->second;

		PACKET(NETPLAY_CHECKSUM, {
			DATA(checksum);
			SEND_TO(m_peers[0]->address(), false);
		});

		it = checksums.erase(it);
		if (it == checksums.end())
			break;
	}
}

bool netplay_manager::verify_checksums()
{
	netplay_assert(m_host);

	auto me = my_peer();
	netplay_assert(me != nullptr);

	auto& my_checksums = me->m_checksums;
	auto target_frame = m_frame_count - 51;

	bool resync = false;

	for (auto it = my_checksums.begin(); it != my_checksums.end(); ++it)
	{
		if (it->first >= target_frame)
			continue;

		for (auto& peer : m_peers)
		{
			if (peer->self())
				continue;

			auto it2 = peer->m_checksums.find(it->first);
			if (it2 == peer->m_checksums.end())
				continue;

			auto checksum = it2->second;
			peer->m_checksums.erase(it2);

			if (it->second != checksum)
			{
				NETPLAY_LOG("checksum mismatch at %d", it->first);
				resync = true;
				break;
			}
		}

		it = my_checksums.erase(it);
		if (it == my_checksums.end())
			break;

		if (resync)
			break;
	}

	if (!resync)
		return false;

	for (auto& peer : m_peers)
		peer->m_checksums.clear();

	send_sync(false);
	return true;
}

netplay_state* netplay_manager::get_state_for(netplay_frame frame)
{
	for (auto& state : m_states)
	{
		if (state.m_frame_count == frame)
			return &state;
	}

	return nullptr;
}

netplay_state* netplay_manager::get_oldest_state()
{
	netplay_state* oldest_state = nullptr;

	for (auto& state : m_states)
		if (oldest_state == nullptr || state.m_frame_count < oldest_state->m_frame_count)
			oldest_state = &state;
	return oldest_state;
}

netplay_state* netplay_manager::get_newest_state()
{
	netplay_state* newest_state = nullptr;

	for (auto& state : m_states)
		if (newest_state == nullptr || state.m_frame_count > newest_state->m_frame_count)
			newest_state = &state;
	return newest_state;
}

bool netplay_manager::can_save()
{
	if (!machine().scheduler().can_save())
		return false;

	for (auto& peer : m_peers)
	{
		if (peer->self() || !peer->dirty())
			continue;

		return false;
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
		
		// m_states is a buffer of previous states used for rollback
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

void netplay_manager::write_packet_header(netplay_socket_writer& writer, unsigned char flags, bool timestamps)
{
	writer.header('P', 'A', 'K', 'T');
	writer.write(m_sync_generation);
	writer.write(flags);

	auto me = my_peer();
	netplay_assert(me != nullptr);
	me->m_last_system_time = system_time();

	if (timestamps)
	{
		writer.write((unsigned char)m_peers.size());
		for(auto& peer : m_peers)
		{
			writer.write(peer->m_peerid);
			writer.write(peer->m_last_system_time);
		}
	}
	else
	{
		unsigned char zero = 0;
		writer.write(zero);
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

	unsigned char num_peers;
	reader.read(num_peers);

	auto me = my_peer();
	netplay_assert(me != nullptr);
	
	for (auto i = 0; i < num_peers; i++)
	{
		unsigned char peerid;
		attotime last_system_time;

		reader.read(peerid);
		reader.read(last_system_time);

		if (peerid != me->m_peerid)
		{
			auto peer = get_peer_by_peerid(peerid);
			if (peer != nullptr)
				peer->m_last_system_time = last_system_time;
			continue;
		}

		// this is my timestamp, calculate the latency to the sender
		auto latency_ms = (system_time() - last_system_time).as_double() * 1000.0;

		static float sum = 0.0f;

		if (m_frame_count % 5 == 0)
		{
			// add half the round-trip latency to this peer's stats
			sender.latency_estimator().add_sample((sum / 5.0f) * 0.5f);
			sum = 0.0f;
		}
		else
		{
			sum += latency_ms;
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
	ss << "frames waited for inputs = " << m_stats.m_waited_for_inputs << "\n";
	ss << "----------------------------\n";

	auto s = ss.str();
	NETPLAY_LOG(s.c_str());
}
