#ifndef MAME_EMU_NETPLAY_INPUT_H
#define MAME_EMU_NETPLAY_INPUT_H

#define NETPLAY_MAX_ANALOG_PORTS 8
#define NETPLAY_MAX_INPUT_PORTS 8

struct netplay_analog_port
{
	int m_accum;
	int m_previous;
	int m_sensitivity;
	bool m_reverse;
	
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

	netplay_analog_port& add_analog_port(int accum, int previous, int sensitivity, int reverse);

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
	std::vector<netplay_input_port> m_ports;

	// non-serialized
	bool m_consumed;

	netplay_input() {}
	netplay_input(const attotime& timestamp);
	netplay_input_port& add_input_port(int defvalue, int digital);
	attotime calculate_future_time(int latency_ms) const; // when this input state is supposed to take effect
	bool consumed() const { return m_consumed; }
	void set_consumed(bool consumed) { m_consumed = consumed; }

	template <typename StreamWriter>
	void serialize(StreamWriter& writer)
	{
		writer.header('I', 'N', 'P', 'T');
		writer.write(m_timestamp);
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

		unsigned int num_ports;
		reader.read(num_ports);
		m_ports.resize(num_ports);

		for(auto& port : m_ports)
		{
			port.deserialize(reader);
		}
	}
};

#endif
