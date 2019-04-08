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
void netplay_packet_write(StreamWriter& writer, unsigned char flags, unsigned int sync_generation)
{
	writer.header('P', 'A', 'K', 'T');
	writer.write(sync_generation);
	writer.write(flags);
}

template <typename StreamReader>
void netplay_packet_read(StreamReader& reader, unsigned char& flags, unsigned int& sync_generation)
{
	reader.header('P', 'A', 'K', 'T');
	reader.read(sync_generation);
	reader.read(flags);
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
