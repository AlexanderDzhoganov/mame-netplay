#ifndef __NETPLAY_INPUT_STATE_H__
#define __NETPLAY_INPUT_STATE_H__

#define NETPLAY_MAX_ANALOG_PORTS 8
#define NETPLAY_MAX_INPUT_PORTS 8

struct netplay_analog_port
{
	int m_accum;
	int m_previous;
	int m_sensitivity;
	bool m_reverse;
};

struct netplay_input_port
{
	unsigned int m_defvalue;
	unsigned int m_digital;
	std::vector<netplay_analog_port> m_analog_ports;

	netplay_analog_port& add_analog_port(int accum, int previous, int sensitivity, int reverse);
};

struct netplay_input_state
{
	netplay_input_state(const attotime& timestamp);

	netplay_input_port& add_input_port(int defvalue, int digital);

	// returns when this input state is supposed to take effect
	attotime calculate_future_time(int latency_ms) const;

	bool consumed() const { return m_consumed; }
	void set_consumed(bool consumed ) { m_consumed = consumed; }

	std::vector<netplay_input_port> m_ports;
	attotime m_timestamp;
	bool m_consumed;
};

#endif
