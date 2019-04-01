#ifndef __NETPLAY_PEER_H__
#define __NETPLAY_PEER_H__

class netplay_peer
{
public:
	netplay_peer(const std::string& name, const netplay_address& address, attotime join_time, bool self = false);
	void add_input_state(std::unique_ptr<netplay_input_state> input_state);

	std::vector<netplay_input_state*> get_inputs_before(attotime before_time);
	void delete_inputs_before(attotime before_time);

	bool self() const { return m_self; }
	const std::string& get_name() const { return m_name; }
	void set_name(const std::string& name) { m_name = name; }
	attotime get_join_time() const { return m_join_time; }
	void set_join_time(const attotime& join_time) { m_join_time = join_time; }
	const netplay_address& get_address() const { return m_address; }
	const std::list<std::shared_ptr<netplay_input_state>>& get_inputs() const { return m_inputs; }

private:
	bool m_self; // peer is this node
	std::string m_name;
	netplay_address m_address;
	attotime m_join_time;
	std::list<std::shared_ptr<netplay_input_state>> m_inputs;
};

#endif
