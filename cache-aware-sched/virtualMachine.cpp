#include "virtualMachine.h"

VirtualMachine::VirtualMachine(unsigned int key, unsigned int hostID, unsigned int localID, unsigned int cpuAffinity)
{
	m_key = key;
	m_hostID = hostID;
	m_localID = localID;

	// m_name = name;
	m_cpuAffinity = cpuAffinity;

	m_numRetiredInsts = 0.0;
	m_numLLCMisses = 0.0;

	m_state = 0;
}

