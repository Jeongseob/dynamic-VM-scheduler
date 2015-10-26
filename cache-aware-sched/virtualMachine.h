#ifndef _VIRTUAL_MACHINE_
#define _VIRTUAL_MACHINE_

#include <iostream>

using namespace std;

class Compare : unary_function<pair<unsigned int, double>, bool>
{
public:
	bool operator() (const pair<unsigned , double>& lhs, const pair<unsigned, double>& rhs) const
	{
		return lhs.second > rhs.second;
	}
};


class VirtualMachine{

public:
	VirtualMachine(unsigned int key, unsigned int hostID, unsigned int localID, unsigned int cpuAffinity);
	//VirtualMachine(unsigned int key, unsigned int hostID, unsigned int localID, string name, string cpuAffinity);
	~VirtualMachine();

	unsigned int		getKey()	{ return m_key;	}
	//string	getName() { return m_name; }
	unsigned int		getHostID()	{ return m_hostID; }
	unsigned int		getLocalID()	{ return m_localID; }
	unsigned int		getCPUAffinity()	{ return m_cpuAffinity; }
	double	getNumRetiredInsts()	{ return m_numRetiredInsts; }
	double	getNumLLCMisses()	{ return m_numLLCMisses; }
	int		getState()		{  return m_state; }

	void	setHostID(unsigned int hostID)		{	m_hostID = hostID; }
	void	setLocalID(unsigned int localID)	{	m_localID = localID; }
	void	setCPUAffinity(unsigned int cpuAffinity)	{	m_cpuAffinity = cpuAffinity; }
	void	setNumRetiredInsts(double numRetiredInsts)	{ m_numRetiredInsts = numRetiredInsts; }
	void	setNumLLCMisses(double numLLCMisses)	{ m_numLLCMisses = numLLCMisses; }
	void	setState(int state)	{ m_state = state; }


private:
	unsigned int	m_key;
	//string			m_name;		// Key
	unsigned int	m_hostID;
	unsigned int	m_localID;

	unsigned int	m_cpuAffinity;
	double			m_numRetiredInsts;
	double			m_numLLCMisses;
	int				m_state;		// 0: Running 1:Migrating
};


#endif
