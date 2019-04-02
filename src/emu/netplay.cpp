#include <string>
#include <sstream>

#include "emu.h"
#include "emuopts.h"
#include "logmacro.h"

#include "netplay_memory.h"
#include "netplay_input.h"
#include "netplay_serialization.h"
#include "netplay_packet.h"
#include "netplay_peer.h"
#include "netplay_socket.h"

#define SEND_PACKET(ADDRESS, FLAGS, ...) do { netplay_socket_stream stream;                 \
	                                      netplay_socket_writer writer(stream);                \
	                                      netplay_pkt_write(writer, m_machine_time, FLAGS); \
	                                      do { __VA_ARGS__ } while(0);                                \
	                                      m_socket->send(stream, ADDRESS); } while(0)          ;

//-------------------------------------------------
// netplay_manager
//-------------------------------------------------

netplay_manager::netplay_manager(running_machine& machine) : 
	 m_machine(machine), m_initialized(false),
	 m_sync_generation(0), m_sync_time(1, 0)
{
	// these defaults can get overidden by config options

	auto host_address = m_machine.options().netplay_host();

	// common
	m_debug = true;                                 // netplay debug printing
	m_host = strlen(host_address) == 0;          // is this node the host
	m_max_block_size = 1024 * 1024;                 // 1 mb max block size
	m_input_freq_ms = 20;                           // 20ms input poll frequency

	// server only
	m_sync_every = 10;                              // sync every 10 seconds

	// client only
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
		netplay_address address = netplay_socket::str_to_addr(host_string);
		if (m_socket->connect(address) != NETPLAY_NO_ERR)
		{
			NETPLAY_LOG("socket failed to connect");
			return false;
		}

		add_peer("server", address);

		// if we're a client then send a handshake packet
		SEND_PACKET(address, NETPLAY_HANDSHAKE,
			netplay_handshake handshake;
			handshake.m_name = "client";
			netplay_pkt_add_handshake(writer, handshake);
		);
	}

	m_initialized = true;
	return true;
}

void netplay_manager::update()
{
	netplay_assert(m_initialized);

	if (m_machine_time.seconds() < 1)
	{
		// still in early startup phase, don't do anything
		return;
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
	if (machine().scheduler().can_save() && m_machine_time.seconds() >= m_sync_time.seconds())
	{
		// if we're past the sync deadline store a sync point
		store_sync();
		m_sync_time += attotime(m_sync_every, 0);
	}

	netplay_socket_stream stream;
	netplay_address sender;
	while (m_socket->receive(stream, sender))
	{
		netplay_socket_reader reader(stream);

		unsigned int flags;
		attotime timestamp;
		netplay_pkt_read(reader, timestamp, flags);

		if (flags & NETPLAY_HANDSHAKE)
		{
			netplay_handshake handshake;
			netplay_pkt_read_handshake(reader, handshake);

			// create a new peer from the handshake data
			auto peer = add_peer(handshake.m_name, sender);

			// send the initial sync
			send_initial_sync(*peer);

			// create a sync object to track the sync
			m_sync_objects.push_back(std::make_shared<netplay_sync>(peer, m_sync_generation, true));

			// pause the machine until the sync is done
			machine().pause();
		}
		
		if (flags & NETPLAY_INPUT)
		{
			auto input = std::make_unique<netplay_input>();
			netplay_pkt_read_input(reader, *input);
			add_input_state(std::move(input));
		}
	}
}

void netplay_manager::update_client()
{
	netplay_socket_stream stream;
	netplay_address sender;
	while (m_socket->receive(stream, sender))
	{
		netplay_socket_reader reader(stream);

		// first read the packet header
		unsigned int flags;
		attotime timestamp;
		netplay_pkt_read(reader, timestamp, flags);

		if (flags & NETPLAY_INPUT)
		{
			// update with any new inputs
			auto input = std::make_unique<netplay_input>();
			netplay_pkt_read_input(reader, *input);
			add_input_state(std::move(input));
		}

		// update all memory blocks if any
		netplay_pkt_copy_blocks(reader, m_sync_blocks);

		if (flags & NETPLAY_INITIAL_SYNC)
		{
			// this is the initial sync so this packet contains all memory blocks
			// we can sync right now and notify the server

			NETPLAY_LOG("received initial sync, restoring state");

			load_sync();

			NETPLAY_LOG("notifying server that we have synced");
			SEND_PACKET(sender, NETPLAY_SYNC_COMPLETE);
		}
	}
}

// create the next sync point
// increment the sync generation
// copy over data from active blocks to sync blocks

void netplay_manager::store_sync()
{
	if (!machine().scheduler().can_save())
	{
		NETPLAY_LOG("(WARNING) cannot store_sync() because scheduler().can_save() == false");
		return;
	}

	netplay_assert(m_host); // only the host should be creating sync points

	if (m_debug)
	{
		NETPLAY_LOG("creating new sync point %zu at time %d", m_sync_generation, m_sync_time.seconds());
	}

	// tell devices to save their memory
	machine().save().dispatch_presave();

	for (auto i = 0; i < m_active_blocks.size(); i++)
	{
		auto& active_block = m_active_blocks[i];
		active_block->set_generation(m_sync_generation);

		auto& sync_block = m_sync_blocks[i];

		// if block checksums don't match or this is the first sync
		// make sure to copy over the data and mark blocks as dirty
		if (sync_block->checksum() != active_block->checksum() || m_sync_generation == 0)
		{
			sync_block->copy_from(*active_block);
			sync_block->set_dirty(true);
		}
		else
		{
			sync_block->set_dirty(false);
		}
	}

	// increment the sync generation and record the time
	m_sync_generation++;
	m_sync_time = m_machine_time;

	// tell devices to load their memory
	machine().save().dispatch_postload();

	// clean up unnecessary inputs
	cleanup_inputs();

	if (m_debug)
	{
		print_debug_info();
	}
}

// load the latest sync point
// copy over data from sync blocks to active blocks

void netplay_manager::load_sync()
{
	if (!machine().scheduler().can_save())
	{
		NETPLAY_LOG("(WARNING) cannot load_sync() because scheduler().can_save() == false");
		return;
	}

	m_machine_time = m_sync_time;

	// tell devices to save their memory
	machine().save().dispatch_presave();

	netplay_assert(m_sync_blocks.size() == m_active_blocks.size());

	for (auto i = 0; i < m_active_blocks.size(); i++)
	{
		m_active_blocks[i]->copy_from(*m_sync_blocks[i]);
	}

	// tell devices to load their memory
	machine().save().dispatch_postload();

	// clean up unnecessary inputs
	cleanup_inputs();

	if (m_debug)
	{
		print_debug_info();
	}
}

void netplay_manager::send_initial_sync(const netplay_peer& peer)
{
	SEND_PACKET(peer.address(), NETPLAY_INITIAL_SYNC,
		for (auto& block : m_sync_blocks)
		{
			netplay_pkt_add_block(writer, *block);
		}
	)
}

bool netplay_manager::socket_connected(const netplay_address& address)
{
	auto addr = netplay_socket::addr_to_str(address);
	NETPLAY_LOG("received socket connection from %s", addr.c_str());

	if (m_host)
	{
		// if we're at max capacity then reject the connection
		// otherwise do nothing and wait for the handshake
		return m_peers.size() < MAX_PLAYERS;
	}

	return true;
}

void netplay_manager::socket_disconnected(const netplay_address& address)
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

		auto sync_block = std::make_shared<netplay_memory>(index, name, block_size);
		m_sync_blocks.push_back(sync_block);

		sync_block->copy_from(*active_block);

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

	auto& self = m_peers[0]; // first peer is always this node's
	self->add_input_state(std::move(input_state));
}

std::shared_ptr<netplay_peer> netplay_manager::add_peer(const std::string& name, const netplay_address& address, bool self)
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
		return m_peers.back();
	}

	NETPLAY_LOG("peer %s with address %s already exists, assuming reconnected", name.c_str(), address_s.c_str());
	existing_peer->set_name(name);
	existing_peer->set_join_time(m_machine_time);
	return existing_peer;
}

void netplay_manager::cleanup_inputs()
{
	// we can safely clean up all inputs before the last sync
	// because we'll never need them even in case of a rollback
	for (auto& peer : m_peers)
	{
		peer->delete_inputs_before(m_sync_time);
	}
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
