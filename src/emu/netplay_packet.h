#ifndef MAME_EMU_NETPLAY_PACKET_H
#define MAME_EMU_NETPLAY_PACKET_H

enum netplay_pkt_flags
{
	NETPLAY_HANDSHAKE =     1 << 0,
	NETPLAY_INITIAL_SYNC =  1 << 1,
	NETPLAY_SYNC_COMPLETE = 1 << 2,
	NETPLAY_INPUT =         1 << 3
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

template <typename StreamWriter>
void netplay_pkt_write(StreamWriter& writer, const attotime& timestamp, unsigned int flags)
{
	writer.header('P', 'A', 'K', 'T');
	writer.write(flags);
	writer.write(timestamp);
}

template <typename StreamReader>
void netplay_pkt_read(StreamReader& reader, attotime& timestamp, unsigned int& flags)
{
	reader.header('P', 'A', 'K', 'T');
	reader.read(flags);
	reader.read(timestamp);
}

template <typename StreamWriter>
void netplay_pkt_add_handshake(StreamWriter& writer, const netplay_handshake& handshake)
{
	handshake.serialize(writer);
}

template <typename StreamReader>
void netplay_pkt_read_handshake(StreamReader& reader, netplay_handshake& handshake)
{
	handshake.deserialize(reader);
}

template <typename StreamWriter>
void netplay_pkt_add_input(StreamWriter& writer, const netplay_input& input)
{
	input.serialize(writer);
}

template <typename StreamReader>
void netplay_pkt_read_input(StreamReader& reader, netplay_input& input)
{
	input.deserialize(reader);
}

template <typename StreamWriter>
void netplay_pkt_add_block(StreamWriter& writer, const netplay_memory& block)
{
	writer.header('B', 'L', 'O', 'K');
	writer.write(block.index());
	writer.write(block.generation());
	writer.write((unsigned int)block.size());
	writer.write(block.data(), block.size());
}

template <typename StreamReader>
void netplay_pkt_copy_blocks(StreamReader& reader, const netplay_blocklist& blocks)
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
		netplay_assert(generation >= block->generation());

		block->set_generation(generation);
		reader.read(block->data(), size);
	}
}

#endif
