#include <string>
#include <sstream>

#include "emu.h"
#include "emuopts.h"
#include "logmacro.h"
#include "ui/uimain.h"

#ifdef EMSCRIPTEN
#include <emscripten.h>
#endif

#include "netplay_util.h"
#include "netplay_memory.h"
#include "netplay_input.h"
#include "netplay_serialization.h"
#include "netplay_packet.h"
#include "netplay_peer.h"
#include "netplay_socket.h"

#define BEGIN_PACKET(FLAGS)                         \
	netplay_socket_stream stream;                     \
	netplay_socket_writer writer(stream);             \
	netplay_packet_write(writer, system_time(), FLAGS);

#define END_PACKET(ADDR) do { m_socket->send(stream, ADDR); } while(0);

//-------------------------------------------------
// netplay_manager
//-------------------------------------------------

const attotime netplay_frametime(0, HZ_TO_ATTOSECONDS(60));

netplay_manager::netplay_manager(running_machine& machine) : 
	 m_machine(machine),
	 m_initialized(false),
	 m_machine_time(0, 0),
	 m_frame_count(0),
	 m_catching_up(false),
	 m_waiting_for_client(false)
{
	auto host_address = m_machine.options().netplay_host();

	// configuration options
	m_debug = true;                     // netplay debug printing
	m_host = strlen(host_address) == 0; // is this node the host
	m_max_block_size = 512 * 1024;      // 512kb max block size
	m_sync_every = attotime(5, 0);      // sync every 1 second

	for (auto i = 0; i < m_states.capacity(); i++)
	{
		netplay_state state;
		m_states.push_back(state);
	}
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

		add_peer("server", address);

		// if we're a client then send a handshake packet
		{
			BEGIN_PACKET(NETPLAY_HANDSHAKE);
				netplay_handshake handshake;
				handshake.m_name = "client";
				handshake.serialize(writer);
			END_PACKET(address);
		}
	}

	m_initialized = true;
	return true;
}

void netplay_manager::update()
{
	netplay_assert(m_initialized);

	if (machine().scheduled_event_pending())
	{
		return;
	}

	auto target_emu_time = machine().time() + netplay_frametime;

	auto can_make_progress = true;
	if (m_waiting_for_client || machine().paused())
		can_make_progress = false;

	for (auto& peer : m_peers)
	{
		if (peer->self())
			continue;
		
		if (peer->get_inputs_for(m_frame_count - 1) == nullptr)
		{
			can_make_progress = false;
			break;
		}
	}

	if (can_make_progress)
	{
		while (machine().time() < target_emu_time)
		{
			machine().scheduler().timeslice();
		}

		m_frame_count++;
		m_machine_time += netplay_frametime;
	}

	if (m_machine_time.seconds() < 1)
	{
		// still in early startup phase, don't do anything
		return;
	}

	auto can_save = machine().scheduler().can_save();
	auto last_sync_time = m_states.newest().m_timestamp;

	if (can_save && m_machine_time >= last_sync_time + m_sync_every)
	{
		// if we're past the sync deadline store a sync point
		store_state();

		/*if (!m_host)
		{
			BEGIN_PACKET(NETPLAY_SYNC_CHECK);
				netplay_sync_check sync_check;
				sync_check.m_generation = m_sync_generation;
				sync_check.m_checksum = memory_checksum(m_sync_blocks);
				sync_check.serialize(writer);
			END_PACKET(m_peers[1]->address());
		}*/
	}

	if (m_host)
	{
		update_host();
	}
	else
	{
		update_client();
	}
}

void netplay_manager::update_host()
{
	netplay_socket_stream stream;
	netplay_addr sender;
	while (m_socket->receive(stream, sender))
	{
		netplay_socket_reader reader(stream);

		unsigned int flags;
		attotime packet_time;
		netplay_packet_read(reader, packet_time, flags);

		if (flags & NETPLAY_HANDSHAKE)
		{
			netplay_handshake handshake;
			handshake.deserialize(reader);

			// new client joined
			// create a new sync for them
			if (machine().scheduler().can_save())
			{
				store_state();
			}

			// add the peer to our list and send the initial sync
			auto& peer = add_peer(handshake.m_name, sender);
			send_full_sync(peer);

			// pause the machine until the sync is done
			m_waiting_for_client = true;
			machine().ui().popup_time(10, "Syncing with new client");
		}
		else if (flags & NETPLAY_SYNC_ACK)
		{
			m_waiting_for_client = false;
		}
		else if (flags & NETPLAY_SYNC_CHECK)
		{
			netplay_sync_check sync_check;
			sync_check.deserialize(reader);

			/*auto checksum = memory_checksum(m_sync_blocks);

			if (sync_check.m_checksum != checksum)
			{
				NETPLAY_LOG("detected desync, doing a full sync");

				auto peer = get_peer_by_addr(sender);
				if (peer != nullptr)
				{
					send_full_sync(*peer);
					m_waiting_for_client = true;
				}
			}*/
		}
		else if (flags & NETPLAY_INPUTS)
		{
			auto input = std::make_unique<netplay_input>();
			input->deserialize(reader);

			auto peer = get_peer_by_addr(sender);
			if (peer != nullptr)
			{
				peer->add_input_state(std::move(input));
			}
		}
	}
}

void netplay_manager::update_client()
{
	netplay_socket_stream stream;
	netplay_addr sender;
	while (m_socket->receive(stream, sender))
	{
		netplay_socket_reader reader(stream);

		// first read the packet header
		unsigned int flags;
		attotime packet_time;
		netplay_packet_read(reader, packet_time, flags);

		if (flags & NETPLAY_INPUTS)
		{
			auto input = std::make_unique<netplay_input>();
			input->deserialize(reader);

			for (auto& peer : m_peers)
			{
				if (peer->address() == sender)
				{
					peer->add_input_state(std::move(input));
				}
			}
		}
		else if (flags & NETPLAY_SYNC)
		{
			netplay_sync sync;
			sync.deserialize(reader);

			m_states.offset_cursor(1);
			auto& state = m_states.newest();

			state.m_generation = sync.m_generation;
			state.m_timestamp = sync.m_sync_time;
			state.m_frame_count = sync.m_frame_count;

			NETPLAY_LOG("received sync %d", sync.m_generation);

			// copy the memory blocks over
			netplay_packet_copy_blocks(reader, state.m_blocks);

			// restore the machine state
			load_state(state);

			NETPLAY_LOG("sync finished at frame %lld and time %s", m_frame_count, m_machine_time.as_string(4));

			BEGIN_PACKET(NETPLAY_SYNC_ACK);
			END_PACKET(sender);
		}
	}
}

// create the next sync point
// increment the sync generation
// copy over data from active blocks to sync blocks

void netplay_manager::store_state()
{
	if (m_debug && !machine().scheduler().can_save())
	{
		NETPLAY_LOG("(WARNING) cannot store_state() because scheduler().can_save() == false");
		return;
	}

	if (m_debug)
	{
		NETPLAY_LOG("sync at %s", m_machine_time.as_string(4));
	}

	auto& prev_state = m_states.newest();

	// move to the next buffer in the states list
	m_states.offset_cursor(1);

	auto& state = m_states.newest();
	auto& blocks = state.m_blocks;

	// tell devices to save their memory
	machine().save().dispatch_presave();

	// copy over the blocks from active memory to the new state
	for (auto i = 0; i < m_active_blocks.size(); i++)
	{
		blocks[i]->copy_from(*m_active_blocks[i]);
	}

	// record metadata
	state.m_generation = prev_state.m_generation + 1;
	state.m_timestamp = m_machine_time;
	state.m_frame_count = m_frame_count;

	// tell devices to load their memory
	machine().save().dispatch_postload();
}

// load the latest sync point
// copy over data from sync blocks to active blocks
void netplay_manager::load_state(const netplay_state& state)
{
	if (m_debug && !machine().scheduler().can_save())
	{
		NETPLAY_LOG("(WARNING) cannot load_state() because scheduler().can_save() == false");
		return;
	}

	m_machine_time = state.m_timestamp;

	// tell devices to save their memory
	machine().save().dispatch_presave();

	netplay_assert(state.m_blocks.size() == m_active_blocks.size());

	for (auto i = 0; i < m_active_blocks.size(); i++)
	{
		m_active_blocks[i]->copy_from(*state.m_blocks[i]);
	}

	// tell devices to load their memory
	machine().save().dispatch_postload();

	if (m_debug)
	{
		print_debug_info();
	}
}

// performs a rollback which first restores the latest sync state we have
// then advances the simulation to the current time while replaying all buffered inputs since then
bool netplay_manager::rollback(const attotime& to_time)
{
	assert(to_time <= m_machine_time); // impossible to go to the future

	if (m_debug)
	{
		NETPLAY_LOG("rollback at %s", m_machine_time.as_string(4));
	}

	// store the current time, we'll advance the simulation to it later
	auto current_time = m_machine_time;

	netplay_state* rollback_state = nullptr;
	// find the newest state we can rollback to
	auto state_time = to_time;
	for (auto& state : m_states)
	{
		if (state.m_timestamp <= state_time)
		{
			rollback_state = &state;
			state_time = state.m_timestamp;
		}
	}

	if (rollback_state == nullptr)
	{
		// the given time is too fast in the past and there is no state to rollback to
		// bail out and let the caller figure it out
		return false;
	}

	// restore the machine to the state we got
	load_state(*rollback_state);

	// figure out how much we have to simulate to get back to the present
	auto delta_time = current_time - m_machine_time;
	auto emu_time = machine().time();

	// let the emulator know we need to catch up
	// this should disable sound and video updates during the simulation loop below
	m_catching_up = true;

	// run the emulator loop
	// this will replay any new inputs that we have received since
	// and sync us up with the other clients
	while (machine().time() < emu_time + delta_time)
	{
		machine().scheduler().timeslice();
	}

	m_catching_up = false;
	return true;
}

void netplay_manager::send_full_sync(const netplay_peer& peer)
{
	BEGIN_PACKET(NETPLAY_SYNC);
		auto& state = m_states.newest();

		netplay_sync sync;
		sync.m_generation = state.m_generation;
		sync.m_sync_time = state.m_timestamp;
		sync.m_frame_count = m_frame_count;
		sync.serialize(writer);

		NETPLAY_LOG("sending initial sync");
		NETPLAY_LOG("sync_time = %s", state.m_timestamp.as_string(4));
		NETPLAY_LOG("frame_count = %lld", m_frame_count);

		for (auto& block : state.m_blocks)
		{
			netplay_packet_add_block(writer, *block);
		}
	END_PACKET(peer.address());
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

	return true;
}

void netplay_manager::socket_disconnected(const netplay_addr& address)
{
	auto addr = netplay_socket::addr_to_str(address);
	NETPLAY_LOG("socket %s disconnected", addr.c_str());

	for (auto it = m_peers.begin(); it != m_peers.end(); ++it)
	{
		if ((*it)->address() == address)
		{
			NETPLAY_LOG("peer %s disconnected", (*it)->name().c_str());
			m_peers.erase(it);

			break;
		}
	}
}

void netplay_manager::create_memory_block(const std::string& name, void* data_ptr, size_t size)
{
	netplay_assert(m_initialized);
	netplay_assert(data_ptr != nullptr);
	netplay_assert(size > 0);

	if (m_debug)
	{
		NETPLAY_LOG("creating %d memory blocks for %s with size %zu",
			std::max(1, (int)(size / m_max_block_size)), name.c_str(), size);
	}

	auto ptr = (unsigned char*)data_ptr;

	while(size > 0)
	{
		auto index = m_active_blocks.size();
		auto block_size = std::min(size, m_max_block_size);

		auto active_block = std::make_shared<netplay_memory>(index, name, ptr, block_size);
		m_active_blocks.push_back(active_block);

		for (auto& state : m_states)
		{
			auto sync_block = std::make_shared<netplay_memory>(index, name, block_size);
			sync_block->copy_from(*active_block);
			state.m_blocks.push_back(sync_block);
		}

		ptr += block_size;
		size -= block_size;
	}

	netplay_assert(size == 0);
}

void netplay_manager::add_input_state(std::unique_ptr<netplay_input> input_state)
{
	netplay_assert(m_initialized);
	netplay_assert(input_state != nullptr);
	netplay_assert(!m_peers.empty());

	for(auto& peer : m_peers)
	{
		if (peer->self())
			continue;

		BEGIN_PACKET(NETPLAY_INPUTS)
			input_state->serialize(writer);
		END_PACKET(peer->address());
	}

	auto& self = m_peers[0]; // first peer is always this node's
	self->add_input_state(std::move(input_state));
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
		m_peers.push_back(std::make_shared<netplay_peer>(name, address, m_machine_time, self));
		return *m_peers.back();
	}

	NETPLAY_LOG("peer %s with address %s already exists, assuming reconnected", name.c_str(), address_s.c_str());
	existing_peer->m_name = name;
	existing_peer->m_join_time = m_machine_time;
	return *existing_peer;
}

void netplay_manager::print_debug_info()
{
	NETPLAY_LOG("memory blocks = %zu", m_active_blocks.size());

	size_t total_mem = 0u;
	for (auto& block : m_active_blocks)
	{
		total_mem += block->size();
	}

	NETPLAY_LOG("total memory = %zu bytes", total_mem);

	auto checksum = memory_checksum(m_active_blocks);
	NETPLAY_LOG("memory checksum = %u", checksum);

	/*for (auto it = m_active_blocks.begin(); it != m_active_blocks.end(); ++it)
	{
		auto block_debug = (*it)->get_debug_string();
		NETPLAY_LOG(block_debug.c_str());
	}*/
}

unsigned char netplay_manager::memory_checksum(const netplay_blocklist& blocks)
{
	unsigned char checksum = 0;

	for (auto& block : blocks)
	{
		checksum ^= block->checksum();
	}

	return checksum;
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
	{
		if (peer->address() == address)
		{
			return peer.get();
		}
	}

	return nullptr;
}
