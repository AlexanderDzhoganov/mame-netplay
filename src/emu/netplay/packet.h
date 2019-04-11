#ifndef MAME_EMU_NETPLAY_PACKET_H
#define MAME_EMU_NETPLAY_PACKET_H

enum netplay_packet_flags
{
	// reliable
	NETPLAY_HANDSHAKE  = 1 << 0, // handshake
	NETPLAY_READY      = 1 << 1, // client ready
	NETPLAY_SYNC       = 1 << 2, // sync data
	// unreliable
	NETPLAY_INPUTS     = 1 << 4, // player inputs
	NETPLAY_CHECKSUM   = 1 << 5  // memory checksum
};

struct netplay_handshake
{
	std::string m_name;                // the human-readable name of this node
	unsigned int m_sync_generation;    // current sync generation
	std::vector<netplay_addr> m_peers; // peer addresses

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.header('H', 'E', 'L', 'O');
		writer.write(m_name);
		writer.write(m_sync_generation);

		writer.write((unsigned int)m_peers.size());
		for (auto& peer : m_peers)
		{
			auto addr = netplay_socket::addr_to_str(peer);
			writer.write(addr);
		}
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.header('H', 'E', 'L', 'O');
		reader.read(m_name);
		reader.read(m_sync_generation);

		unsigned int num_peers;
		reader.read(num_peers);
		m_peers.resize(num_peers);

		for (auto i = 0; i < num_peers; i++)
		{
			std::string addr;
			reader.read(addr);
			m_peers[i] = netplay_socket::str_to_addr(addr);
		}
	}
};

struct netplay_ready
{
	std::string m_name;

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.header('R', 'E', 'D', 'Y');
		writer.write(m_name);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.header('R', 'E', 'D', 'Y');
		reader.read(m_name);
	}
};

struct netplay_sync
{
	netplay_frame m_frame_count; // frame count at sync
	unsigned int m_input_delay;  // current input delay value

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.header('S', 'Y', 'N', 'C');
		writer.write(m_frame_count);
		writer.write(m_input_delay);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.header('S', 'Y', 'N', 'C');
		reader.read(m_frame_count);
		reader.read(m_input_delay);
	}
};

struct netplay_checksum
{
	netplay_frame m_frame_count;           // frame index of the latest state
	std::vector<unsigned int> m_checksums; // block checksums
	bool m_processed;                      // (non-serialized) whether we've processed these checksums
	netplay_addr m_peer_address;           // (non-serialized) the address of the peer who sent this checksum

	netplay_checksum() : m_frame_count(0), m_processed(false) {}

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.header('C', 'H', 'E', 'K');
		writer.write(m_frame_count);
		
		writer.write((unsigned int)m_checksums.size());
		writer.write((void*)m_checksums.data(), m_checksums.size() * sizeof(unsigned int));
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.header('C', 'H', 'E', 'K');
		reader.read(m_frame_count);
	
		unsigned int checksums_size;
		reader.read(checksums_size);
		m_checksums.resize(checksums_size);
		reader.read((void*)m_checksums.data(), m_checksums.size() * sizeof(unsigned int));
	}
};

struct netplay_set_delay
{
	netplay_frame m_frame_count;
	unsigned int m_input_delay;
	bool m_processed; // (non-serialized)

	netplay_set_delay() : m_frame_count(0), m_input_delay(0), m_processed(true) {}

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.header('D', 'L', 'A', 'Y');
		writer.write(m_frame_count);
		writer.write(m_input_delay);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.header('D', 'L', 'A', 'Y');
		reader.read(m_frame_count);
		reader.read(m_input_delay);
	}
};

template <typename StreamWriter>
void netplay_packet_add_block(StreamWriter& writer, const netplay_memory& block)
{
	writer.header('B', 'L', 'O', 'K');
	writer.write(block.index());
	writer.write((unsigned int)block.size());
	writer.write(block.data(), block.size());
}

template <typename StreamReader>
void netplay_packet_read_blocks(StreamReader& reader, const netplay_blocklist& blocks)
{
	unsigned int index;
	unsigned int size;

	while(!reader.eof())
	{
		reader.header('B', 'L', 'O', 'K');
		reader.read(index);
		reader.read(size);

		netplay_assert(index < blocks.size());

		auto& block = blocks[index];
		netplay_assert(size == block->size());

		reader.read(block->data(), size);
		block->invalidate_checksum();
	}
}

#endif
