#ifndef MAME_EMU_NETPLAY_PACKET_H
#define MAME_EMU_NETPLAY_PACKET_H

enum netplay_packet_flags
{
	// reliable
	NETPLAY_HANDSHAKE  = 1 << 0, // handshake
	NETPLAY_READY      = 1 << 1, // client ready
	NETPLAY_SYNC       = 1 << 2, // sync data
	NETPLAY_DELAY      = 1 << 3, // new input delay
	NETPLAY_CHECKSUM   = 1 << 4, // client memory checksum
	// unreliable
	NETPLAY_INPUTS     = 1 << 5 // player inputs
};

struct netplay_handshake
{
	std::string m_name;                // the human-readable name of this node
	unsigned int m_sync_generation;    // current sync generation
	unsigned char m_peerid;            // the client's new peer id
	std::vector<std::pair<unsigned char, netplay_addr>> m_peers; // peer ids and addresses

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.header('H', 'E', 'L', 'O');
		writer.write(m_name);
		writer.write(m_sync_generation);
		writer.write(m_peerid);

		writer.write((unsigned int)m_peers.size());
		for (auto& pair : m_peers)
		{
			writer.write(pair.first);
			pair.second.serialize(writer);
		}
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.header('H', 'E', 'L', 'O');
		reader.read(m_name);
		reader.read(m_sync_generation);
		reader.read(m_peerid);

		unsigned int num_peers;
		reader.read(num_peers);
		m_peers.resize(num_peers);

		for (auto i = 0; i < num_peers; i++)
		{
			unsigned char peerid;
			reader.read(peerid);

			netplay_addr address;
			address.deserialize(reader);
			m_peers[i] = std::make_pair(peerid, address);
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
	unsigned int m_input_delay;

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

struct netplay_delay
{
	netplay_frame m_effective_frame;
	unsigned int m_input_delay;
	bool m_processed; // non-serialized

	netplay_delay() : m_effective_frame(0), m_input_delay(0), m_processed(true) {}

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.header('D', 'L', 'A', 'Y');
		writer.write(m_effective_frame);
		writer.write(m_input_delay);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.header('D', 'L', 'A', 'Y');
		reader.read(m_effective_frame);
		reader.read(m_input_delay);
	}
};

struct netplay_checksum
{
	netplay_frame m_frame_count;
	std::vector<unsigned int> m_checksums;

	netplay_checksum() : m_frame_count(0) {}

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.header('C', 'H', 'E', 'K');
		writer.write(m_frame_count);

		writer.write((unsigned int)m_checksums.size());
		for (auto& checksum : m_checksums)
			writer.write(checksum);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.header('C', 'H', 'E', 'K');
		reader.read(m_frame_count);

		unsigned int num_checksums;
		reader.read(num_checksums);
		m_checksums.resize(num_checksums);

		for (auto i = 0; i < num_checksums; i++)
			reader.read(m_checksums[i]);
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
	}
}

#endif
