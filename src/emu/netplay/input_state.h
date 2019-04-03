#ifndef MAME_EMU_NETPLAY_INPUT_H
#define MAME_EMU_NETPLAY_INPUT_H

struct netplay_analog_port
{
	int m_accum;
	int m_previous;
	int m_sensitivity;
	bool m_reverse;

	bool operator==(const netplay_analog_port& port) const
	{
		// TODO FIXME: is this correct?
		return memcmp(this, &port, sizeof(netplay_analog_port)) == 0;
	}

	bool operator!=(const netplay_analog_port& port) const { return !(port == *this); }
	
	template <typename StreamWriter>
	void serialize(StreamWriter& writer)
	{
		writer.write(m_accum);
		writer.write(m_previous);
		writer.write(m_sensitivity);
		writer.write(m_reverse);
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.read(m_accum);
		reader.read(m_previous);
		reader.read(m_sensitivity);
		reader.read(m_reverse);
	}
};

struct netplay_input_port
{
	unsigned int m_defvalue;
	unsigned int m_digital;
	std::vector<netplay_analog_port> m_analog_ports;

	netplay_analog_port& add_analog_port(int accum, int previous, int sensitivity, bool reverse);

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

		return m_defvalue == port.m_defvalue && m_digital == port.m_digital;
	}

	bool operator!=(const netplay_input_port& port) const { return !(port == *this); };

	template <typename StreamWriter>
	void serialize(StreamWriter& writer)
	{
		writer.write(m_defvalue);
		writer.write(m_digital);

		writer.write((unsigned int)m_analog_ports.size());
		for(auto& analog_port : m_analog_ports)
		{
			analog_port.serialize(writer);
		}
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.read(m_defvalue);
		reader.read(m_digital);

		unsigned int num_analog_ports;
		reader.read(num_analog_ports);
		m_analog_ports.resize(num_analog_ports);

		for(auto& analog_port : m_analog_ports)
		{
			analog_port.deserialize(reader);
		}
	}
};

struct netplay_input
{
	attotime m_timestamp;
	unsigned long long m_frame_index; // the frame index to which this input applies
	std::vector<netplay_input_port> m_ports;
	
	// non serialized
 
	netplay_input() {}
	netplay_input(const attotime& timestamp, unsigned long long frame_index);
	netplay_input_port& add_input_port(int defvalue, int digital);

	bool operator==(const netplay_input& input) const
	{
		if (m_ports.size() != input.m_ports.size())
			return false;

		for (auto i = 0; i < m_ports.size(); i++)
		{
			if (m_ports[i] != input.m_ports[i])
			{
				return false;
			}
		}

		return true;
	}

	bool operator!=(const netplay_input& port) const { return !(port == *this); };

	template <typename StreamWriter>
	void serialize(StreamWriter& writer)
	{
		writer.header('I', 'N', 'P', 'T');
		writer.write(m_timestamp);
		writer.write(m_frame_index);
		writer.write((unsigned int)m_ports.size());

		for(auto& port : m_ports)
		{
			port.serialize(writer);
		}
	}

	template <typename StreamReader>
	void deserialize(StreamReader& reader)
	{
		reader.header('I', 'N', 'P', 'T');
		reader.read(m_timestamp);
		reader.read(m_frame_index);

		unsigned int num_ports;
		reader.read(num_ports);
		m_ports.resize(num_ports);

		for(auto& port : m_ports)
		{
			port.deserialize(reader);
		}
	}

	std::string debug_string() const
	{
		std::stringstream ss;

		ss << "input buffer\n";
		ss << "num_ports = " << m_ports.size() << "\n";

		for (auto i = 0; i < m_ports.size(); i++)
		{
			auto& port = m_ports[i];
			ss << "- port #" << i << "\n";
			ss << "- num_analog = " << port.m_analog_ports.size() << "\n";

			for (auto q = 0; q < port.m_analog_ports.size(); q++)
			{
				ss << "- - analog #" << q << "\n";
			}
		}

		return ss.str();
	}
};

#endif
