#ifndef MAME_EMU_NETPLAY_PACKET_H
#define MAME_EMU_NETPLAY_PACKET_H

enum netplay_packet_flags
{
	NETPLAY_HANDSHAKE  = 1 << 0, // handshake
	NETPLAY_SYNC       = 1 << 1, // sync data
	NETPLAY_INPUTS     = 1 << 2, // player inputs
	NETPLAY_CHECKSUM   = 1 << 3, // memory checksum
	NETPLAY_SET_DELAY  = 1 << 4, // set input delay
	NETPLAY_PING       = 1 << 5, // ping
	NETPLAY_PONG       = 1 << 6, // ping response
};

struct netplay_handshake
{
	std::string m_name;

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.write(m_name);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.read(m_name);
	}
};

struct netplay_sync
{
	attotime m_timestamp;        // machine time at which the sync occurred
	netplay_frame m_frame_count; // frame count at sync
	unsigned int m_input_delay;  // current input delay value

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.header('S', 'Y', 'N', 'C');
		writer.write(m_timestamp);
		writer.write(m_frame_count);
		writer.write(m_input_delay);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.header('S', 'Y', 'N', 'C');
		reader.read(m_timestamp);
		reader.read(m_frame_count);
		reader.read(m_input_delay);
	}
};

struct netplay_checksum
{
	netplay_frame m_frame_count;             // frame index of the latest state
	std::vector<unsigned short> m_checksums; // block checksums
	bool m_processed;                        // (non-serialized) whether we've processed these checksums

	netplay_checksum() : m_frame_count(0), m_processed(false) {}

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.header('C', 'H', 'E', 'K');
		writer.write(m_frame_count);
		
		writer.write((unsigned int)m_checksums.size());
		for(auto checksum : m_checksums)
			writer.write(checksum);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.header('C', 'H', 'E', 'K');
		reader.read(m_frame_count);
	
		unsigned int checksums_size;
		reader.read(checksums_size);
		m_checksums.resize(checksums_size);

		for (auto i = 0; i < checksums_size; i++)
			reader.read(m_checksums[i]);
	}
};

template <typename StreamWriter>
void netplay_packet_write(StreamWriter& writer, unsigned int flags, unsigned int sync_generation)
{
	writer.header('P', 'A', 'K', 'T');
	writer.write(flags);
	writer.write(sync_generation);
}

template <typename StreamReader>
void netplay_packet_read(StreamReader& reader, unsigned int& flags, unsigned int& sync_generation)
{
	reader.header('P', 'A', 'K', 'T');
	reader.read(flags);
	reader.read(sync_generation);
}

template <typename StreamWriter>
void netplay_packet_add_block(StreamWriter& writer, const netplay_memory& block)
{
	writer.header('B', 'L', 'O', 'K');
	writer.write(block.index());
	writer.write((unsigned int)block.size());
	writer.write(block.data(), block.size());
}

template <typename StreamReader>
void netplay_packet_copy_blocks(StreamReader& reader, const netplay_blocklist& blocks)
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
