#ifndef MAME_EMU_NETPLAY_INPUT_H
#define MAME_EMU_NETPLAY_INPUT_H

struct netplay_analog_port
{
	unsigned int m_accum;
	unsigned int m_previous;

	netplay_analog_port() : m_accum(0), m_previous(0) {}

	bool operator==(const netplay_analog_port& port) const
	{
		return m_accum == port.m_accum && m_previous == port.m_previous;
	}

	bool operator!=(const netplay_analog_port& port) const { return !(port == *this); }
	
	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.write(m_accum);
		writer.write(m_previous);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.read(m_accum);
		reader.read(m_previous);
	}
};

struct netplay_input_port
{
	unsigned int m_digital;
	std::vector<netplay_analog_port> m_analog;

	unsigned int m_defvalue; // non-serialized, only for local player

	netplay_input_port() : m_digital(0), m_defvalue(0) {}

	bool operator==(const netplay_input_port& port) const
	{
		if (m_analog.size() != port.m_analog.size())
			return false;

		for (auto i = 0; i < m_analog.size(); i++)
			if (m_analog[i] != port.m_analog[i])
				return false;

		return m_digital == port.m_digital;
	}

	bool operator!=(const netplay_input_port& port) const { return !(port == *this); };

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.write(m_digital);

		/*netplay_assert(m_analog.size() <= 8);
		unsigned char mask = 0;
		for (auto i = 0u; i < m_analog.size(); i++)
			if (m_analog[i].m_accum != 0 || m_analog[i].m_previous != 0)
				mask |= (1 << i);

		writer.write(mask);

		for(auto& analog : m_analog)
			if (analog.m_accum != 0 || analog.m_previous != 0)
				analog.serialize(writer);*/
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.read(m_digital);

		/*unsigned char mask;
		reader.read(mask);

		unsigned int num_analog = 0;
		for (auto i = 0u; i < 8; i++)
			if (mask & (1 << i))
				num_analog++;

		m_analog.resize(num_analog);

		for (auto i = 0u; i < 8; i++)
			if (mask & (1 << i))
				m_analog[i].deserialize(reader);*/
	}
};

struct netplay_input
{
	std::vector<netplay_input_port> m_ports;
	netplay_frame m_frame_index; // (non serialized) the frame index to which this input applies
	
	netplay_input() : m_frame_index(0) { m_ports.reserve(16); }

	bool operator==(const netplay_input& input) const
	{
		if (m_ports.size() != input.m_ports.size())
			return false;

		for (auto i = 0; i < m_ports.size(); i++)
			if (m_ports[i] != input.m_ports[i])
				return false;

		return true;
	}

	bool operator!=(const netplay_input& port) const { return !(port == *this); };

	template <typename StreamWriter>
	void serialize(StreamWriter& writer) const
	{
		writer.header('I', 'N', 'P', 'T');
		
		netplay_assert(m_ports.size() <= 24);

		unsigned int mask = 0;
		mask |= (unsigned char)m_ports.size();

		for (auto i = 0u; i < m_ports.size(); i++)
			if (m_ports[i].m_digital != 0)
				mask |= (1 << (i + 8));

		writer.write(mask);

		for(auto& port : m_ports)
			if (port.m_digital != 0)
				port.serialize(writer);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.header('I', 'N', 'P', 'T');

		unsigned int mask;
		reader.read(mask);

		auto num_ports = mask & 0xFF;
		m_ports.resize(num_ports);

		for (auto i = 0u; i < 32; i++)
			if (mask & (1 << (i + 8)))
				m_ports[i].deserialize(reader);
	}

	std::string debug_string() const;
};

#endif
