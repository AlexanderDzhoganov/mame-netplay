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
	void serialize(StreamWriter& writer)
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
	std::vector<netplay_analog_port> m_analog_ports;

	netplay_input_port() : m_digital(0) {}

	netplay_analog_port& add_analog_port(int accum, int previous);

	bool operator==(const netplay_input_port& port) const
	{
		if (m_analog_ports.size() != port.m_analog_ports.size())
			return false;

		for (auto i = 0; i < m_analog_ports.size(); i++)
		{
			if (m_analog_ports[i] != port.m_analog_ports[i])
			{
				return false;
			}
		}

		return m_digital == port.m_digital;
	}

	bool operator!=(const netplay_input_port& port) const { return !(port == *this); };

	template <typename StreamWriter>
	void serialize(StreamWriter& writer)
	{
		writer.write(m_digital);

		netplay_assert(m_analog_ports.size() <= 255);
		writer.write((unsigned char)m_analog_ports.size());

		for(auto& analog_port : m_analog_ports)
			analog_port.serialize(writer);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.read(m_digital);

		unsigned char num_analog_ports;
		reader.read(num_analog_ports);
		m_analog_ports.resize(num_analog_ports);

		for(auto& analog_port : m_analog_ports)
			analog_port.deserialize(reader);
	}
};

struct netplay_input
{
	netplay_frame m_frame_index; // the frame index to which this input applies
	std::vector<netplay_input_port> m_ports;
	
	netplay_input() : m_frame_index(0) { m_ports.reserve(16); }
	netplay_input_port& add_input_port(int digital);

	bool operator==(const netplay_input& input) const
	{
		if (m_ports.size() != input.m_ports.size())
			return false;

		for (auto i = 0; i < m_ports.size(); i++)
		{
			if (m_ports[i] != input.m_ports[i])
				return false;
		}

		return true;
	}

	bool operator!=(const netplay_input& port) const { return !(port == *this); };

	template <typename StreamWriter>
	void serialize(StreamWriter& writer)
	{
		writer.header('I', 'N', 'P', 'T');
		writer.write(m_frame_index);
		
		netplay_assert(m_ports.size() <= 255);
		writer.write((unsigned char)m_ports.size());

		for(auto& port : m_ports)
			port.serialize(writer);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.header('I', 'N', 'P', 'T');
		reader.read(m_frame_index);

		unsigned char num_ports;
		reader.read(num_ports);
		m_ports.resize(num_ports);

		for(auto& port : m_ports)
			port.deserialize(reader);
	}

	std::string debug_string() const;
};

#endif
