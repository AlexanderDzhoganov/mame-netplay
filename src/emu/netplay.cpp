#include <string>
#include <sstream>
#include <unistd.h>

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

#define BEGIN_PACKET(FLAGS)                         \
	netplay_socket_stream stream;                     \
	netplay_socket_writer writer(stream);             \
	netplay_packet_write(writer, system_time(), FLAGS);

#define END_PACKET(ADDR) do { m_socket->send(stream, ADDR); } while(0);

//-------------------------------------------------
// netplay_manager
//-------------------------------------------------

// const attotime netplay_frametime(0, HZ_TO_ATTOSECONDS(60));

netplay_manager::netplay_manager(running_machine& machine) : 
	 m_machine(machine),
	 m_initialized(false),
	 m_catching_up(false),
	 m_waiting_for_client(false),
	 m_rollback(false),
	 m_rollback_frame(0),
	 m_initial_sync(false),
	 m_checksum_frame(0),
	 m_frame_count(0)
{
	auto host_address = m_machine.options().netplay_host();

	// configuration options
	m_debug = true; // netplay debug printing
	m_host = strlen(host_address) == 0; // is this node the host
	m_max_block_size = 512 * 1024; // 512kb max block size
	m_input_delay = 3; // use 3 frames of input delay
	m_checksum_every = 100; // checksum every 100 frames

	for (auto i = 0; i < m_states.capacity(); i++)
	{
		m_states.push_back(netplay_state());
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

	if (!m_waiting_for_client && !machine().paused())
	{
		// auto target_time = machine().time() + netplay_frametime;
		auto target_frame = m_frame_count + 1;

		while (m_frame_count < target_frame)
			machine().scheduler().timeslice();

		if (machine().scheduler().can_save())
			store_state();
	}

	if (m_host)
		update_host();
	else
		update_client();

	if (m_rollback)
	{
		if (!rollback(m_rollback_frame) && m_host)
		{
			NETPLAY_LOG("rollback failed, resyncing all clients");
			m_waiting_for_client = true;

			for(auto& peer : m_peers)
			{
				if (peer->self())
					continue;

				send_full_sync(*peer);
			}
		}

		m_rollback = false;
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

		if (flags & NETPLAY_SYNC_ACK)
		{
			m_waiting_for_client = false;
		}

		if (flags & NETPLAY_HANDSHAKE)
		{
			netplay_handshake handshake;
			handshake.deserialize(reader);

			// new client joined, create a new sync state for them
			if (machine().scheduler().can_save())
			{
				store_state();
			}

			// add the peer to our list and send the initial sync
			auto& peer = add_peer(handshake.m_name, sender);
			
			if (m_debug)
				NETPLAY_LOG("initial sync with %s at %lld", peer.name().c_str(), m_frame_count);

			send_full_sync(peer);

			// wait until the sync is done
			m_waiting_for_client = true;
		}

		// if we just resynced the client and she hasn't acknowledged yet
		// skip all packets because they contain old data
		if (m_waiting_for_client && !(flags & NETPLAY_SYNC_ACK))
		{
			continue;
		}

		if (flags & NETPLAY_INPUTS)
		{
			auto input = std::make_unique<netplay_input>();
			input->deserialize(reader);
			auto peer = get_peer_by_addr(sender);
			if (peer != nullptr)
			{
				handle_input(std::move(input), *peer);
			}
		}
		else if (flags & NETPLAY_CHECKSUM)
		{
			netplay_checksum checksum;
			checksum.deserialize(reader);

			auto peer = get_peer_by_addr(sender);
			if (peer == nullptr)
			{
				continue;
			}

			bool state_found = false;
			for (auto& state : m_states)
			{
				if(state.m_frame_count != checksum.m_frame_count)
				{
					continue;
				}

				state_found = true;
				auto& blocks = state.m_blocks;
				assert(blocks.size() == checksum.m_checksums.size());
				
				for (auto i = 0; i < blocks.size(); i++)
				{
					if (blocks[i]->checksum() == checksum.m_checksums[i])
					{
						continue;
					}

					NETPLAY_LOG("checksum mismatch in block %s", blocks[i]->name().c_str());
				}

				// send_full_sync(*peer);
				// m_waiting_for_client = true;
			}

			if (!state_found)
			{
				NETPLAY_LOG("checksum: failed to find state for frame %llu", checksum.m_frame_count);
			}
		}
	}
}

void netplay_manager::update_client()
{
	if (m_frame_count >= m_checksum_frame)
	{
		auto& state = m_states.newest();
		auto& blocks = state.m_blocks;

		BEGIN_PACKET(NETPLAY_CHECKSUM)
			netplay_checksum checksum;
			checksum.m_frame_count = state.m_frame_count;
			checksum.m_checksums.resize(blocks.size());
			for (auto i = 0; i < blocks.size(); i++)
			{
				checksum.m_checksums[i] = blocks[i]->checksum();
			}

			checksum.serialize(writer);
		END_PACKET(m_peers[1]->address());

		// send a checksum packet every N frames
		m_checksum_frame = m_frame_count + m_checksum_every;
	}

	netplay_socket_stream stream;
	netplay_addr sender;
	while (m_socket->receive(stream, sender))
	{
		auto peer = get_peer_by_addr(sender);
		if (peer == nullptr)
		{
			continue;
		}

		netplay_socket_reader reader(stream);

		// first read the packet header
		unsigned int flags;
		attotime packet_time;
		netplay_packet_read(reader, packet_time, flags);

		if (flags & NETPLAY_SYNC)
		{
			netplay_sync sync;
			sync.deserialize(reader);
			handle_sync(sync, reader, *peer);
		}
		else if (flags & NETPLAY_INPUTS)
		{
			auto input = std::make_unique<netplay_input>();
			input->deserialize(reader);
			handle_input(std::move(input), *peer);
		}
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
	{
		m_memory[i]->copy_from(*state.m_blocks[i]);
	}

	// tell devices to load their memory
	machine().save().dispatch_postload();
}

// performs a rollback which first restores the latest sync state we have
// then advances the simulation to the current time while replaying all buffered inputs since then
bool netplay_manager::rollback(unsigned long long before_frame)
{
	netplay_assert(before_frame <= m_frame_count); // impossible to go to the future

	// store the current time, we'll advance the simulation to it later
	auto start_frame = m_frame_count;
	auto start_time = system_time();

	netplay_state* rollback_state = nullptr;
	unsigned long long state_frame = 0;

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
		NETPLAY_LOG("rollback failed: no state available at frame %llu", before_frame);
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

	if (m_debug)
	{
		auto duration = system_time() - start_time;
		NETPLAY_LOG("rollback from %llu to %llu (took %dms)",
			start_frame, before_frame, (int)(duration.as_double() * 1000));
	}

	return true;
}

// sends a full sync to the selected peer
// this is slow and expensive and is used as a last resort in case of a desync
// also used to initialize newly connected peers
void netplay_manager::send_full_sync(const netplay_peer& peer)
{
	BEGIN_PACKET(NETPLAY_SYNC);
		auto& state = m_states.newest();

		netplay_sync sync;
		sync.m_sync_time = state.m_timestamp;
		sync.m_frame_count = m_frame_count;
		sync.serialize(writer);

		for (auto& block : state.m_blocks)
		{
			netplay_packet_add_block(writer, *block);
		}
	END_PACKET(peer.address());
}

void netplay_manager::handle_sync(const netplay_sync& sync, netplay_socket_reader& reader, netplay_peer& peer)
{
	if (m_debug)
		NETPLAY_LOG("sync at frame %lld and time %s", sync.m_frame_count, sync.m_sync_time.as_string(4));

	m_states.advance(1);

	auto& state = m_states.newest();
	state.m_timestamp = sync.m_sync_time;
	state.m_frame_count = sync.m_frame_count;

	// copy the memory blocks over
	netplay_packet_copy_blocks(reader, state.m_blocks);

	// restore the machine state
	load_state(state);

	// acknowledge that we have caught up
	BEGIN_PACKET(NETPLAY_SYNC_ACK);
	END_PACKET(peer.address());

	if (!m_host)
	{
		m_initial_sync = true;
		m_checksum_frame = m_frame_count + m_checksum_every;
	}
}

// handle inputs coming over the network
void netplay_manager::handle_input(std::unique_ptr<netplay_input> input_state, netplay_peer& peer)
{
	netplay_assert(input_state != nullptr);

	auto effective_frame = input_state->m_frame_index + m_input_delay;

	// NETPLAY_LOG("received input: frame = %llu, effective = %llu, time = %s, my_time = %s, my_frame = %llu",
		// input_state->m_frame_index, effective_frame, input_state->m_timestamp.as_string(4), machine().time().as_string(4), m_frame_count);

	// if the input is in the future it means we're falling behind
	// record it and move on
	if (effective_frame > m_frame_count)
	{
		// add the input to the peer's buffer and bail out
		peer.add_input_state(std::move(input_state));
		return;
	}

	// get any predicted input we used in place of this one
	auto predicted_input = peer.get_predicted_inputs_for(effective_frame);

	// test if both the actual and predicted inputs are the same
	// if the predicted inputs differ then we do a rollback to just before the input time
	if (predicted_input == nullptr || *predicted_input != *input_state)
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

		// add this input state to the peer's buffers so it can be used during the rollback
		peer.add_input_state(std::move(input_state));
	}
}

bool netplay_manager::socket_connected(const netplay_addr& address)
{
	auto addr = netplay_socket::addr_to_str(address);
	NETPLAY_LOG("received socket connection from %s", addr.c_str());

	if (m_host)
	{
		machine().ui().popup_time(5, "A new peer is connecting...");

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
			machine().ui().popup_time(5, "Peer '%s' has disconnected.", (*it)->name().c_str());
			NETPLAY_LOG("peer %s disconnected", (*it)->name().c_str());
			m_peers.erase(it);
			break;
		}
	}
}

// called by save_manager whenever a device creates a memory block for itself
// populates the corresponding netplay sync blocks
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
		auto index = m_memory.size();
		auto block_size = std::min(size, m_max_block_size);

		auto active_block = std::make_shared<netplay_memory>(index, name, ptr, block_size);
		m_memory.push_back(active_block);

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

// called by ioport every update with the new input state
void netplay_manager::add_input_state(std::unique_ptr<netplay_input> input_state)
{
	if ((!m_host && !m_initial_sync) || m_frame_count <= m_input_delay)
		return;
	
	netplay_assert(m_initialized);
	netplay_assert(input_state != nullptr);
	netplay_assert(!m_peers.empty());

	// send the inputs to all peers
	for(auto& peer : m_peers)
	{
		if (peer->self())
			continue;

		BEGIN_PACKET(NETPLAY_INPUTS)
			input_state->serialize(writer);
		END_PACKET(peer->address());
	}

	// add the inputs to our own buffer
	m_peers[0]->add_input_state(std::move(input_state));
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
	{
		if (peer->address() == address)
		{
			return peer.get();
		}
	}

	return nullptr;
}

void netplay_manager::print_debug_info()
{
	NETPLAY_LOG("memory blocks = %zu", m_memory.size());

	size_t total_mem = 0u;
	for (auto& block : m_memory)
	{
		total_mem += block->size();
	}

	NETPLAY_LOG("total memory = %zu bytes", total_mem);

	auto checksum = netplay_memory::checksum(m_memory);
	NETPLAY_LOG("memory checksum = %u", checksum);

	/*for (auto it = m_memory.begin(); it != m_memory.end(); ++it)
	{
		auto block_debug = (*it)->get_debug_string();
		NETPLAY_LOG(block_debug.c_str());
	}*/
}
