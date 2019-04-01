#include <string>
#include <sstream>

#include "emu.h"
#include "emuopts.h"
#include "logmacro.h"

#include "netplay_memory_block.h"
#include "netplay_input_state.h"
#include "netplay_stream.h"
#include "netplay_packet.h"
#include "netplay_peer.h"
#include "netplay_socket.h"

//-------------------------------------------------
// netplay_manager
//-------------------------------------------------

netplay_manager::netplay_manager(running_machine& machine) : 
	 m_initialized(false), m_machine(machine), m_sync_generation(0)
{
	// these defaults can get overidden by config options

	// common
	m_debug = true;                  // netplay debug printing
	m_hosting = true;                // is this node the host
	m_max_block_size = 1024 * 1024;  // 1 mb max block size

	// server only
	m_sync_every = 10;               // sync every 10 seconds
	m_sync_time = attotime(1, 0);    // first sync at 1 second
	// client only
}

bool netplay_manager::initialize()
{
	assert(!m_initialized);
	NETPLAY_LOG("initializing netplay");

	m_socket = std::make_unique<netplay_socket>(*this);

	if (m_hosting)
	{
		netplay_listen_socket listen_socket;
		if (m_socket->listen(listen_socket) != netplay_no_err)
		{
			NETPLAY_LOG("failed to open listen socket");
			return false;
		}
	}
	else
	{ 
		netplay_address address;
		if (m_socket->connect(address) != netplay_no_err)
		{
			NETPLAY_LOG("socket failed to connect");
			return false;
		}
	}

	auto self_address = m_socket->get_self_address();
	add_peer(m_hosting ? "server" : "client", self_address, true);
	
	m_initialized = true;
	return true;
}

void netplay_manager::update()
{
	assert(m_initialized);

	if (m_machine_time.seconds() < 1)
	{
		// still in early startup phase, don't do anything
		return;
	}

	if (m_hosting)
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
	auto next_sync_time = m_sync_time.seconds() + m_sync_every;
	if (m_machine_time.seconds() >= next_sync_time)
	{
		store_sync_point();
	}

	netplay_packet packet;
	netplay_address sender;
	while (m_socket->receive(packet, sender))
	{
		if (packet.has_handshake())
		{
			auto& handshake = packet.get_handshake();
			add_peer(handshake.m_name, sender);
		}
	}
}

void netplay_manager::update_client()
{
	netplay_packet packet;
	netplay_address sender;
	while (m_socket->receive(packet, sender))
	{
	}
}

// create the next sync point
// increment the sync generation
// copy over data from active blocks to sync blocks

void netplay_manager::store_sync_point()
{
	assert(m_hosting); // only the host should be creating sync points

	// increment the sync generation and record the time
	m_sync_generation++;
	m_sync_time = m_machine_time;

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
		if (sync_block->checksum() != active_block->checksum())
		{
			sync_block->copy_from(*active_block);
			sync_block->set_dirty(true);
		}
		else
		{
			sync_block->set_dirty(false);
		}
	}

	// tell devices to load their memory
	machine().save().dispatch_postload();

	// clean up unnecessary inputs
	cleanup_inputs();
}

// load the latest sync point
// copy over data from sync blocks to active blocks

void netplay_manager::load_sync_point()
{
	m_machine_time = m_sync_time;

	// tell devices to save their memory
	machine().save().dispatch_presave();

	for (auto i = 0; i < m_active_blocks.size(); i++)
	{
		auto& active_block = m_active_blocks[i];
		auto& sync_block = m_sync_blocks[i];
		active_block->copy_from(*sync_block);
	}

	// tell devices to load their memory
	machine().save().dispatch_postload();

	// clean up unnecessary inputs
	cleanup_inputs();
}

bool netplay_manager::socket_connected(const netplay_address& address)
{
	auto addr = netplay_socket::address_to_string(address);
	NETPLAY_LOG("received socket connection from %s", addr.c_str());
	return true;
}

void netplay_manager::socket_disconnected(const netplay_address& address)
{
	auto addr = netplay_socket::address_to_string(address);
	NETPLAY_LOG("socket %s disconnected", addr.c_str());

	for (auto it = m_peers.begin(); it != m_peers.end(); ++it)
	{
		if ((*it)->get_address() == address)
		{
			NETPLAY_LOG("peer %s disconnected", (*it)->get_name().c_str());
			m_peers.erase(it);

			break;
		}
	}
}

void netplay_manager::create_memory_block(const std::string& name, void* data_ptr, size_t size)
{
	assert(m_initialized);
	assert(data_ptr != nullptr);
	assert(size > 0);

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

		auto active_block = std::make_shared<netplay_memory_block>(index, name, ptr, block_size);
		m_active_blocks.push_back(active_block);

		auto sync_block = std::make_shared<netplay_memory_block>(index, name, block_size);
		m_sync_blocks.push_back(sync_block);

		sync_block->copy_from(*active_block);

		ptr += block_size;
		size -= block_size;
	}

	assert(size == 0);
}

void netplay_manager::add_input_state(std::unique_ptr<netplay_input_state> input_state)
{
	assert(m_initialized);
	assert(input_state != nullptr);

	auto& self = m_peers[0]; // first peer is always this node's
	self->add_input_state(std::move(input_state));
}

void netplay_manager::add_peer(const std::string& name, const netplay_address& address, bool self)
{
	netplay_peer* existing_peer = nullptr;
	for (auto it = m_peers.begin(); it != m_peers.end(); ++it)
	{
		auto& peer = *it;
		if (peer->get_address() == address)
		{
			existing_peer = peer.get();
			break;
		}
	}

	if (existing_peer == nullptr)
	{
		NETPLAY_LOG("adding new peer %s with address %s", name.c_str(), address.c_str());
		m_peers.push_back(std::make_shared<netplay_peer>(name, address, m_machine_time, self));
	}
	else
	{
		NETPLAY_LOG("peer %s with address %s already exists, assuming reconnected", name.c_str(), address.c_str());
		existing_peer->set_name(name);
		existing_peer->set_join_time(m_machine_time);
	}
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

	size_t total_mem = 0;
	for (auto it = m_active_blocks.begin(); it != m_active_blocks.end(); ++it)
	{
		total_mem += (*it)->get_size();
	}

	NETPLAY_LOG("total memory = %zu bytes", total_mem);

	auto checksum = memory_checksum(m_active_blocks);
	NETPLAY_LOG("memory checksum = %u", checksum);

	for (auto it = m_active_blocks.begin(); it != m_active_blocks.end(); ++it)
	{
		auto block_debug = (*it)->get_debug_string();
		NETPLAY_LOG(block_debug.c_str());
	}
}

unsigned char netplay_manager::memory_checksum(const std::vector<std::shared_ptr<netplay_memory_block>>& blocks)
{
	unsigned char checksum = 0;

	for (auto it = blocks.begin(); it != blocks.end(); ++it)
	{
		checksum ^= (*it)->checksum();
	}

	return checksum;
}
