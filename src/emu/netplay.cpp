#include <string>
#include <sstream>
#include <unistd.h>
#include <memory>

#include "emu.h"
#include "emuopts.h"
#include "osdepend.h"
#include "screen.h"
#include "ui/uimain.h"

#include <emscripten.h>
#include <emscripten/bind.h>

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

netplay_manager* netplay_manager::m_instance = nullptr;

netplay_manager::netplay_manager(running_machine& machine) : 
	m_machine(machine),
	m_initialized(false),
	m_sync_generation(0),
	m_frame_count(1),
	m_catching_up(false),
	m_waiting_for_connection(false),
	m_input_delay_backoff(0),
	m_next_peerid(1),
	m_host_time(0, 0)
{
	netplay_assert(m_instance == nullptr);
	m_instance = this;
	
	auto& opts = m_machine.options();
	auto host_address = opts.netplay_host();

	// configuration options
	m_debug = opts.netplay_debug();     // netplay debug printing
	m_name = opts.netplay_name();       // name of this node
	m_host = strlen(host_address) == 0; // is this node the host

	if (!m_host)
		m_host_address = netplay_socket::str_to_addr(host_address);

	m_input_delay = 5;     // use N frames of input delay
	m_max_rollback = 5;    // max rollback of N frames
	memset(&m_stats, 0, sizeof(netplay_stats));
}

netplay_manager::~netplay_manager()
{
	m_instance = nullptr;
}

bool netplay_manager::initialize()
{
	netplay_assert(!m_initialized);
	NETPLAY_LOG("initializing netplay");

	for (auto& entry : machine().save().m_entry_list)
		create_memory_block(*entry);

	m_socket = std::make_unique<netplay_socket>(*this);

	if (m_host)
	{
		// this node is the host so it gets to be first in the peers list
		auto& me = add_peer(0, m_socket->get_self_address(), true);
		me.m_state = NETPLAY_PEER_ONLINE;
		me.m_name = m_name;

		netplay_listen_socket listen_socket;
		if (m_socket->listen(listen_socket) != NETPLAY_NO_ERR)
			return false;
	}
	else if (m_socket->connect(m_host_address) != NETPLAY_NO_ERR)
	{
		return false;
	}

	EM_ASM_ARGS({ jsmame_netplay_init($0); }, (unsigned int)machine().system().name);
	m_initialized = true;
	return true;
}

void netplay_manager::update()
{
	netplay_assert(m_initialized);

	NETPLAY_VERBOSE_LOG("begin frame %d (last = %#08x, memory = %#08x)",
		m_frame_count, m_checkpoint.checksum(), memory_checksum());

	if (wait_for_connection())
		return;

	if (m_host)
		for (auto& peer : m_peers)
			if (peer->m_state == NETPLAY_PEER_SYNCING)
				return;

	auto& next_delay = m_next_input_delay;
	if (!next_delay.m_processed && next_delay.m_effective_frame == m_frame_count)
	{
		static netplay_frame frames_waited = 0;

		if (!can_save())
		{
			if (m_host && frames_waited++ >= 240)
			{
				NETPLAY_LOG("timed out while waiting for inputs, resyncing...");
				send_sync(false);
				frames_waited = 0;
				return;
			}

			NETPLAY_VERBOSE_LOG("have new input delay but not clean yet, waiting...");
			m_stats.m_waited_for_inputs++;
			return;
		}

		frames_waited = 0;

		set_input_delay(next_delay.m_input_delay);
		next_delay.m_processed = true;
	}

	/*if (m_frame_count > 1000 && m_frame_count % 100 == 1)
	{
		std::vector<unsigned int> checksums;
		for (auto i = 0; i < m_memory.size(); i++)
			checksums.push_back(m_memory[i]->checksum());

		rollback();

		for (auto i = 0; i < m_memory.size(); i++)
			if (m_memory[i]->checksum() != checksums[i])
				NETPLAY_LOG("(!!) checksum mismatch at %d in block %s", m_frame_count, m_memory[i]->module_name().c_str());
	}*/

	static netplay_frame frames_waited = 0;
	if (m_host && frames_waited++ >= 240)
	{
		NETPLAY_LOG("timed out while waiting for inputs, resyncing");
		send_sync(false);
		frames_waited = 0;
		return;
	}
	else if (m_checkpoint.m_frame_count + m_max_rollback < m_frame_count)
	{
		NETPLAY_VERBOSE_LOG("waiting for inputs...");
		m_stats.m_waited_for_inputs++;
		return;
	}

	frames_waited = 0;
	
	simulate_until(m_frame_count + 1);

	// if (m_host)
	//	recalculate_input_delay();

	// every N frames garbage collect the input buffers
	auto gc_every = 30;
	if (m_frame_count % gc_every && m_frame_count > gc_every)
		for (auto& peer : m_peers)
			peer->gc_buffers(m_frame_count - gc_every);

	NETPLAY_VERBOSE_LOG("end frame %d (last = %#08x, memory = %#08x)",
		m_frame_count, m_checkpoint.checksum(), memory_checksum());

	static netplay_frame last_stats_frame = 0;
	if (m_debug && m_frame_count >= last_stats_frame + 2000)
	{
		print_stats();
		memset(&m_stats, 0, sizeof(netplay_stats));
		last_stats_frame = m_frame_count;
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

	m_input_delay_backoff = 120;
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
void netplay_manager::store_state()
{
	// tell devices to save their memory
	machine().save().dispatch_presave();

	netplay_assert(m_checkpoint.m_blocks.size() == m_memory.size());

	// copy over the blocks from active memory to the new state
	for (auto i = 0; i < m_memory.size(); i++)
		m_checkpoint.m_blocks[i]->copy_from(*m_memory[i]);
	
	// record the time
	m_checkpoint.m_frame_count = m_frame_count;

	NETPLAY_VERBOSE_LOG(">> storing state at %d, checksum = %#08x",
		m_checkpoint.m_frame_count, m_checkpoint.checksum());

	/*if (!m_host)
	{
		netplay_checksum checksum;
		checksum.m_frame_count = state.m_frame_count;
		checksum.m_checksums.resize(state.m_blocks.size());
		for (auto i = 0; i < state.m_blocks.size(); i++)
			checksum.m_checksums[i] = state.m_blocks[i]->checksum();

		PACKET(NETPLAY_CHECKSUM, {
			DATA(checksum);
			SEND_TO(m_peers[0]->address(), false);
		});
	}
	else
	{
		auto me = my_peer();
		netplay_assert(me != nullptr);

		std::vector<unsigned int> checksums;
		checksums.resize(state.m_blocks.size());
		for (auto i = 0; i < state.m_blocks.size(); i++)
			checksums[i] = state.m_blocks[i]->checksum();

		me->m_checksums[state.m_frame_count] = checksums;
	}*/
}

// load the latest sync point
// copy over data from sync blocks to active blocks
void netplay_manager::load_state(const netplay_state& state)
{
	netplay_assert(state.m_blocks.size() == m_memory.size());

	m_frame_count = state.m_frame_count;

	// tell devices to save their memory
	machine().save().dispatch_presave();

	for (auto i = 0; i < m_memory.size(); i++)
		m_memory[i]->copy_from(*state.m_blocks[i]);

	// tell devices to load their memory
	machine().save().dispatch_postload();
}

// performs a rollback which first restores the latest sync state we have
// then advances the simulation to the current time while replaying all buffered inputs since then
void netplay_manager::rollback()
{
	// store the current time, we'll advance the simulation to it later
	auto start_frame = m_frame_count;

	NETPLAY_VERBOSE_LOG("(!) rollback from %d to %d (last = %#08x, memory = %#08x)",
		start_frame, m_checkpoint.m_frame_count, m_checkpoint.checksum(), memory_checksum());

	// restore the machine to the last valid state
	load_state(m_checkpoint);

	NETPLAY_VERBOSE_LOG("(!) after load_state (memory = %#08x)", memory_checksum());

	// let the emulator know we need to catch up
	// this should disable sound and video updates during the simulation loop below
	m_catching_up = true;

	// run the emulator loop, this will replay any new inputs that we have received since
	simulate_until(start_frame);

	NETPLAY_VERBOSE_LOG("(!) rollback finished (memory = %#08x)", memory_checksum());

	m_catching_up = false;
	m_stats.m_rollbacks++;
}

void netplay_manager::simulate_until(netplay_frame frame_index)
{
	screen_device* screen = screen_device_iterator(machine().root_device()).first();

	if (m_catching_up)
		machine().sound().system_mute(true);

	while (m_frame_count != frame_index)
	{
		machine().ioport().frame_update();

		auto current_frame = screen->frame_number();
		while (screen->frame_number() == current_frame)
			machine().scheduler().timeslice();

		m_frame_count++;
	}

	if (m_catching_up)
		machine().sound().system_mute(false);
	else
		machine().video().frame_update();

	if (can_save())
		store_state();
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
	m_input_delay_backoff = 120;
	m_input_delay = calculate_input_delay();

	load_state(m_checkpoint);

	for (auto& peer : m_peers)
	{
		peer->m_inputs.clear();
		peer->m_predicted_inputs.clear();
		peer->m_next_inputs_at = m_checkpoint.m_frame_count + m_input_delay + 1;
		peer->m_checksums.clear();

		if (!peer->self())
			peer->m_state = NETPLAY_PEER_SYNCING;
	}

	netplay_socket_writer packet;
	write_packet_header(packet, NETPLAY_SYNC); 

	netplay_sync sync;
	sync.m_frame_count = m_checkpoint.m_frame_count;
	sync.m_input_delay = m_input_delay;
	sync.serialize(packet);

	m_snapshot.m_frame_count = m_checkpoint.m_frame_count;

	for (auto i = 0; i < m_checkpoint.m_blocks.size(); i++)
	{
		auto& block = m_checkpoint.m_blocks[i];
		auto& good_block = m_snapshot.m_blocks[i];

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
		auto checksum = m_snapshot.checksum();
		auto size = packet.stream().cursor();
		NETPLAY_LOG("sending %ssync: frame = %d, checksum = %#08x, size = %lu, next_inputs = %d",
			full_sync ? "full " : "", m_checkpoint.m_frame_count, checksum, size, m_checkpoint.m_frame_count + m_input_delay + 1);
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
		handle_inputs(reader, peer);
	}
	else if (flags & NETPLAY_CHECKSUM)
	{
		netplay_checksum checksum;
		checksum.deserialize(reader);
		peer.m_checksums[checksum.m_frame_count] = checksum.m_checksums;
	}
	else if (flags & NETPLAY_SYNC)
	{
		if (peer.m_state == NETPLAY_PEER_SYNCING)
		{
			peer.m_state = NETPLAY_PEER_ONLINE;
			NETPLAY_VERBOSE_LOG("peer %d is synced", peer.m_peerid);
		}
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
		handle_inputs(reader, peer);
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
	m_snapshot.m_frame_count = sync.m_frame_count;
	m_input_delay = sync.m_input_delay;
	m_next_input_delay.m_processed = true;

	for (auto& peer : m_peers)
	{
		peer->m_inputs.clear();
		peer->m_predicted_inputs.clear();
		peer->m_next_inputs_at = sync.m_frame_count + m_input_delay + 1;
		peer->m_checksums.clear();
	}

	// copy the memory blocks over
	netplay_packet_read_blocks(reader, m_snapshot.m_blocks);

	// restore the machine state
	load_state(m_snapshot);

	// store the new state
	store_state();

	m_stats.m_syncs++;
	m_stats.m_sync_total_bytes += reader.stream().size();

	PACKET(NETPLAY_SYNC, {
		SEND_TO(m_peers[0]->address(), true);
	});

	NETPLAY_LOG("received sync: frame = %d, size = %lu, checksum = %#08x",
		sync.m_frame_count, reader.stream().size(), m_snapshot.checksum());
}

void netplay_manager::handle_inputs(netplay_socket_reader& reader, netplay_peer& peer)
{
	auto& inputs = peer.m_inputs;
	bool do_rollback = false;

	netplay_frame frame_index;
	unsigned char mask;

	reader.read(frame_index);
	reader.read(mask);

	netplay_input input;
	for (auto i = 0u; i < 8; i++)
	{
		if (!(mask & (1 << i)))
			continue;

		input.deserialize(reader);

		auto input_frame = frame_index - i;
		input.m_frame_index = input_frame;

		auto it = inputs.find(input_frame);
		if (it != inputs.end())
			continue;

		NETPLAY_VERBOSE_LOG("received inputs from peer %d for %d", peer.m_peerid, input_frame);
		if (input_frame <= m_checkpoint.m_frame_count)
			NETPLAY_VERBOSE_LOG("input frame = %d predates last state time = %d",
				input_frame, m_checkpoint.m_frame_count);

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
			NETPLAY_VERBOSE_LOG("prediction for %d is OK", input.m_frame_index);
			continue;
		}

		NETPLAY_VERBOSE_LOG("prediction for %d is BAD", input.m_frame_index);
		do_rollback = true;
	}

	if (do_rollback)
		rollback();
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
	peer.m_state = NETPLAY_PEER_NOT_READY;

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
		host.m_state = NETPLAY_PEER_ONLINE;

		// then add ourselves, we take a temporary id of 255 until we learn the actual one
		auto& me = add_peer(255, m_socket->get_self_address(), true);
		me.m_name = m_name;
		me.m_state = NETPLAY_PEER_ONLINE;
		return true;
	}

	// check if we know of this peer
	auto connected_peer = get_peer(address);
	if (connected_peer == nullptr)
	{
		// if we don't know who this is then it's a peer that just connected
		// they take a temporary id of 255 until we get the actual one
		auto& peer = add_peer(255, address);
		peer.m_state = NETPLAY_PEER_ONLINE;
	}
	else
	{
		connected_peer->m_state = NETPLAY_PEER_ONLINE;
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
	auto peer = get_peer(sender);
	if (peer == nullptr)
		return;

	// read the packet header
	unsigned char flags;
	if(!read_packet_header(reader, flags, *peer))
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
	NETPLAY_VERBOSE_LOG("sending inputs for %d", frame_index);

	netplay_socket_writer packet;
	write_packet_header(packet, NETPLAY_INPUTS, true);

	netplay_peer* me = my_peer();
	netplay_assert(me != nullptr);
	auto& inputs = me->inputs();

	packet.write(frame_index);

	unsigned char mask = 0;
	for (auto i = 0; i < 8; i++)
	{
		if (i > frame_index)
			continue;

		auto it = inputs.find(frame_index - i);
		if (it != inputs.end())
			mask |= (1 << i);
	}

	packet.write(mask);

	// send the last 8 input states for redundancy
	for (auto i = 0; i < 8; i++)
	{
		if (!(mask & (1 << i)))
			continue;

		auto it = inputs.find(frame_index - i);
		if (it == inputs.end())
			continue;

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

	m_peers.push_back(std::make_shared<netplay_peer>(peerid, address, self));
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

netplay_peer* netplay_manager::get_peer(const netplay_addr& address) const
{
	for(auto& peer : m_peers)
		if (peer->m_address == address)
			return peer.get();

	return nullptr;
}

netplay_peer* netplay_manager::get_peer(unsigned char peerid) const
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
		if (peer->self() || peer->m_state == NETPLAY_PEER_ONLINE)
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
	NETPLAY_LOG("setting input delay to %d at %d", input_delay, m_frame_count);

	if (input_delay > m_input_delay)
		for (auto& peer : m_peers)
			peer->m_next_inputs_at = m_frame_count + input_delay + 1;
	m_input_delay = input_delay;
}

void netplay_manager::verify_checksums()
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

			netplay_assert(it2->second.size() == it->second.size());
			for (auto i = 0; i < it2->second.size(); i++)
			{
				if (it2->second[i] == it->second[i])
					continue;

				NETPLAY_LOG("checksum mismatch at %d in block %s", it->first, m_memory[i]->module_name().c_str());
				resync = true;
			}

			peer->m_checksums.erase(it2);
		}

		it = my_checksums.erase(it);
		if (it == my_checksums.end())
			break;
	}

	if (!resync)
		return;

	// send_sync(false);
}

bool netplay_manager::can_save()
{
	if (!machine().scheduler().can_save())
		return false;

	auto start_frame = m_checkpoint.m_frame_count;
	auto end_frame = m_frame_count;

	for (auto& peer : m_peers)
	{
		if (peer->m_state != NETPLAY_PEER_ONLINE || peer->self())
			continue;

		if (peer->dirty())
			return false;

		for (auto i = start_frame; i <= end_frame; i++)
		{
			if (peer->m_next_inputs_at > i)
				continue;

			auto it = peer->m_inputs.find(i);
			if (it == peer->m_inputs.end())
				return false;
		}
	}
	
	return true;
}

unsigned int netplay_manager::memory_checksum()
{
	unsigned int checksum = 0;
	for (auto& block : m_memory)
		checksum ^= block->checksum();
	return checksum;
}

static const unsigned int save_game_header = 0xB4DF00D;
static const unsigned int save_game_version = 1;

void netplay_manager::save_game()
{
	netplay_stream_writer<netplay_memory_stream> writer;

	writer.write(save_game_header);
	writer.write(save_game_version);
	m_checkpoint.serialize(writer);

	auto& data = writer.stream().data();

	// force a redraw so we get data in the canvas
	machine().osd().update(false);

	EM_ASM_ARGS({
		jsmame_savegame($0, $1);
	}, (unsigned int)data.data(), data.size());
}

void netplay_manager::load_game(uintptr_t bytes_ptr, int size)
{
	NETPLAY_LOG("loading serialized state (size = %d)", size);
	auto bytes = reinterpret_cast<char*>(bytes_ptr);

	netplay_raw_byte_stream stream(bytes, size);
	netplay_stream_reader<netplay_raw_byte_stream> reader(stream);

	unsigned int header;
	reader.read(header);

	if (header != save_game_header)
	{
		NETPLAY_LOG("invalid or corrupted save game file");
		return;
	}

	unsigned int version;
	reader.read(version);

	if (version != save_game_version)
	{
		NETPLAY_LOG("save game version mismatch, expected %d but got %d", save_game_version, version)
		return;
	}

	m_snapshot.deserialize(reader);
	load_state(m_snapshot);
	store_state();

	NETPLAY_LOG("state restored from serialized buffer");
}

void netplay_manager::create_memory_block(state_entry& entry)
{
	netplay_assert(entry.m_data != nullptr);

	auto size = entry.m_typecount * entry.m_typesize;
	netplay_assert(size > 0);

	auto index = m_memory.size();
	
	// m_memory stores the active blocks currently in-use by the emulator
	auto active_block = std::make_shared<netplay_memory>(index, entry.m_module, entry.m_name, entry.m_device, entry.m_data, size);
	m_memory.push_back(active_block);

	// m_snapshot stores the last acknowledged state between peers used for resync
	auto good_block = std::make_shared<netplay_memory>(index, entry.m_module, entry.m_name, entry.m_device, size);
	good_block->copy_from(*active_block);
	m_snapshot.m_blocks.push_back(good_block);

	auto last_block = std::make_shared<netplay_memory>(index, entry.m_module, entry.m_name, entry.m_device, size);
	last_block->copy_from(*active_block);
	m_checkpoint.m_blocks.push_back(last_block);
}

void netplay_manager::write_packet_header(netplay_socket_writer& writer, unsigned char flags, bool timestamps)
{
	writer.header('P', 'A', 'K', 'T');
	writer.write(m_sync_generation);

	if (timestamps)
		flags |= NETPLAY_TIMESTAMP;

	writer.write(flags);

	if (flags & NETPLAY_TIMESTAMP)
		writer.write(m_host ? system_time() : m_host_time);
}

bool netplay_manager::read_packet_header(netplay_socket_reader& reader, unsigned char& flags, netplay_peer& sender)
{
	reader.header('P', 'A', 'K', 'T');

	unsigned int sync_generation;
	reader.read(sync_generation);
	if (sync_generation < m_sync_generation)
		return false;

	reader.read(flags);

	if (flags & NETPLAY_TIMESTAMP)
	{
		if (m_host)
		{
			attotime last_system_time;
			reader.read(last_system_time);

			if (last_system_time.seconds() != 0)
			{
				auto latency_ms = (system_time() - last_system_time).as_double() * 1000.0;
				sender.latency_estimator().add_sample(latency_ms * 0.5f);
			}
		}
		else
		{
			reader.read(m_host_time);
		}
	}

	return true;
}

void netplay_manager::print_stats() const
{
	std::stringstream ss;

	ss << "----------------------------\n";
	ss << "frame count = " << m_frame_count << "\n";
	ss << "rollbacks = " << m_stats.m_rollbacks << "\n";
	
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

using namespace emscripten;

EMSCRIPTEN_BINDINGS(netplay)
{
	class_<netplay_manager>("netplay")
		.class_function("instance", &netplay_manager::instance, allow_raw_pointers())
		.function("saveGame", &netplay_manager::save_game)
		.function("loadGame", &netplay_manager::load_game, allow_raw_pointers())
	;
}
