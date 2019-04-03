#ifndef MAME_EMU_NETPLAY_PACKET_H
#define MAME_EMU_NETPLAY_PACKET_H

enum netplay_packet_flags
{
	NETPLAY_HANDSHAKE  = 1 << 0, // client->server handshake
	NETPLAY_SYNC       = 1 << 1, // packet contains sync data
	NETPLAY_SYNC_ACK   = 1 << 2, // sync acknowledgement
	NETPLAY_SYNC_CHECK = 1 << 3, // sync check
	NETPLAY_INPUTS     = 1 << 4, // packet contains player inputs
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
	attotime m_sync_time;             // machine time at which the sync occurred
	unsigned long long m_frame_count; // frame count at sync
	int m_generation;                 // sync generation

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.header('S', 'Y', 'N', 'C');
		writer.write(m_sync_time);
		writer.write(m_frame_count);
		writer.write(m_generation);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.header('S', 'Y', 'N', 'C');
		reader.read(m_sync_time);
		reader.read(m_frame_count);
		reader.read(m_generation);
	}
};

struct netplay_sync_check
{
	int m_generation;         // sync generation
	unsigned char m_checksum; // memory checksum
	
	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.write(m_generation);
		writer.write(m_checksum);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.read(m_generation);
		reader.read(m_checksum);
	}
};

template <typename StreamWriter>
void netplay_packet_write(StreamWriter& writer, const attotime& timestamp, unsigned int flags)
{
	writer.header('P', 'A', 'K', 'T');
	writer.write(flags);
	writer.write(timestamp);
}

template <typename StreamReader>
void netplay_packet_read(StreamReader& reader, attotime& timestamp, unsigned int& flags)
{
	reader.header('P', 'A', 'K', 'T');
	reader.read(flags);
	reader.read(timestamp);
}

template <typename StreamWriter>
void netplay_packet_add_block(StreamWriter& writer, const netplay_memory& block)
{
	writer.header('B', 'L', 'O', 'K');
	writer.write(block.index());
	writer.write(block.generation());
	writer.write((unsigned int)block.size());
	writer.write(block.data(), block.size());
}

template <typename StreamReader>
void netplay_packet_copy_blocks(StreamReader& reader, const netplay_blocklist& blocks)
{
	unsigned int index;
	unsigned int size;
	int generation;

	while(!reader.eof())
	{
		reader.header('B', 'L', 'O', 'K');
		reader.read(index);
		reader.read(generation);
		reader.read(size);

		netplay_assert(index < blocks.size());

		auto& block = blocks[index];
		netplay_assert(size == block->size());

		block->set_generation(generation);
		reader.read(block->data(), size);
	}
}

#endif
