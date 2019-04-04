#ifndef MAME_EMU_NETPLAY_PACKET_H
#define MAME_EMU_NETPLAY_PACKET_H

enum netplay_packet_flags
{
	NETPLAY_HANDSHAKE  = 1 << 0, // client->server handshake
	NETPLAY_SYNC       = 1 << 1, // packet contains sync data
	NETPLAY_SYNC_ACK   = 1 << 2, // sync acknowledgement
	NETPLAY_INPUTS     = 1 << 4, // packet contains player inputs
	NETPLAY_CHECKSUM   = 1 << 5, // memory checksum
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

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.header('S', 'Y', 'N', 'C');
		writer.write(m_sync_time);
		writer.write(m_frame_count);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.header('S', 'Y', 'N', 'C');
		reader.read(m_sync_time);
		reader.read(m_frame_count);
	}
};

struct netplay_checksum
{
	unsigned long long m_frame_count;       // frame index of the latest state
	std::vector<unsigned char> m_checksums; // block checksums

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.header('C', 'H', 'E', 'K');
		writer.write(m_frame_count);
		writer.write((unsigned int)m_checksums.size());
		for(auto checksum : m_checksums)
		{
			writer.write(checksum);
		}
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
		{
			reader.read(m_checksums[i]);
		}
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
	}
}

#endif
