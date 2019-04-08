#ifndef MAME_EMU_NETPLAY_ADDR_H
#define MAME_EMU_NETPLAY_ADDR_H

struct netplay_addr
{
	friend class netplay_socket;
	bool operator==(const netplay_addr& address) const { return m_peerid == address.m_peerid; }
	bool operator!=(const netplay_addr& address) const { return !(*this == address); }

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.write(m_peerid);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.read(m_peerid);
	}

	protected:
	std::string m_peerid;
};

struct netplay_listen_socket {};

#endif
