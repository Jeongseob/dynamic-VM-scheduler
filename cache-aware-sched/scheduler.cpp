#include <iostream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <iomanip>

#include <map>
#include <vector>
#include <algorithm>
#include <cmath>

#include <time.h>
#include "virtualMachine.h"
#include "crew.h"

#define LLC_MISS_SAMPLE_THRESHOLD           10000
#define RETIRED_INST_SAMPLE_THRESHOLD       500000

#define LOCAL_LLC_THRESHOLD					50
#define GLOBAL_LLC_THRESHOLD				500

#define LOCAL_SCHD_TIME_INTERVAL			10
#define GLOBAL_SCHD_TIME_INTERVAL			15
#define	NUM_OF_NUMA_NODES					2

using namespace std;

typedef pair<int , int > virtualMachineKey;

struct numaMemoryInfo {
	int	numOfPages[NUM_OF_NUMA_NODES];
};

// Function prototype
void*	migrationHelperThread(void *);
void*	globalWorkerThread(void *);
void*	localWorkerThread(void *);
void	signalHandler(int );
int		initialize(unsigned int );
string	sshCommand(unsigned int , string );
VirtualMachine* getVM(unsigned int );
numaMemoryInfo	getNUMAAffinity(int , int );
unsigned int	getCPUAffinity(VirtualMachine* );
unsigned int	getLocalID(int , VirtualMachine* );
string			migrate(int , int, VirtualMachine*, int node = 0 );
string			setCPUAffinity(int , VirtualMachine* );

// Global variables
multimap<int, VirtualMachine*> g_hostToVM_map;
pthread_mutex_t	g_hostToVM_map_mutex;

map<unsigned int, VirtualMachine*>	g_vmMap;
map<unsigned int, string>			g_vmNameMap;
map<int, double>					g_missRatePerHost;

unsigned int* g_highLLC_VM;
unsigned int* g_lowLLC_VM;
pthread_mutex_t*	g_llc_mutex;

bool*				g_hostMigrating;
pthread_cond_t*		g_hostMigrating_go;
pthread_mutex_t*	g_hostMigrating_mutex;

static crew_t		g_localCrew;
static crew_t		g_globalCrew;
static crew_t		g_migrationCrew;

pthread_mutex_t		g_migration_mutex;
pthread_cond_t		g_migration_done;
int					g_migrationCompleteCnt = 0;

string	g_hostPrefix;
bool g_exitCond = false;
unsigned int g_numHosts = 0;

int main(int argc, char *argv[])
{
	int status;
	
	// Default number of workers 1
	g_numHosts = CREW_SIZE;
		
	if (argc < 3) {
		cerr << "usage: " << argv[0] << " [host_prefix] [number of hosts]" << endl;
		exit(1);
	}

	g_hostPrefix = argv[1];
	g_numHosts = atoi(argv[2]);

	cout << "Host prefix: " << g_hostPrefix << endl;
	cout << "Num of hosts: " << g_numHosts << endl;

	// Initalize
	if ( initialize(g_numHosts) ) {
		cerr << "Failed to initalize the data structures.." << endl;
		exit(1);
	}

	// Create localCrew thread
	status = create_crew(&g_localCrew, g_numHosts, localWorkerThread);
	if ( status != 0 ) {
		cerr << "Failed to create local crew " << endl; 
	}
	
	// Create globalCrew thread
	status = create_crew(&g_globalCrew, 1, globalWorkerThread);
	if ( status != 0 ) {
		cerr << "Failed to create global crew " << endl; 
	}

	// Create migrationHelper thread
	status = create_crew(&g_migrationCrew, 1, migrationHelperThread);
	if ( status != 0 ) {
		cerr << "Failed to create migrationHelper crew " << endl; 
	}

	// Signal handling
	struct sigaction sa;
	sa.sa_handler = signalHandler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);

	// Wait crew thread
	wait_crew(&g_localCrew);
	wait_crew(&g_globalCrew);
	wait_crew(&g_migrationCrew);

	cout << "Close... " << endl;
	return 0;
}

void signalHandler(int signo)
{
	if ( signo == SIGINT ) {
		cout << "All Thread will be terminated due to SIGINT signal " << endl;
		g_exitCond = true;

		sleep(3);
		
		cout << "Request to cancnel migration threads" << endl;
		if ( pthread_cancel(g_globalCrew.worker[0].thread) != 0 ) {
			cout << "Cannot kill the global thread" << endl;
		}

		if ( pthread_cancel(g_migrationCrew.worker[0].thread) != 0 ) {
		
			cout << "Cannot kill the migrationHelper thread" << endl;
		}
		
		for ( int i = 0; i < g_localCrew.worker_size; i++ ) {
			
			cout << "Request to cancel thread[" << i << "]" << endl;

			if ( pthread_cancel(g_localCrew.worker[i].thread) != 0 ) {
				cerr << "Cannot kill the local thread[ " << i << "]" << endl;
			}
		}

	}
}

int initialize(unsigned int nHosts)
{
	ostringstream os;
	string remoteCmd;
	string ret;
	unsigned int numOfVMs;
	unsigned int theKey = 0;

	cout << "Initalizing... " << endl;

	// check all hosts
	for (unsigned int hostID = 1; hostID <= nHosts; hostID++) 
	{
		// 1. obtain number of virtual machines
		remoteCmd = "";
		remoteCmd = "xl list | wc -l";
		numOfVMs = atoi(sshCommand(hostID, remoteCmd).c_str()) - 2;

		for (unsigned int j = 0; j < numOfVMs; j++) {

			// 2. obtain name for each virtual machine
			remoteCmd = "";
			remoteCmd = "xl list | awk 'NR==";
			stringstream vmid;
			vmid << (j+3) << " {print $1}'";
			string vmName = sshCommand(hostID, remoteCmd + vmid.str());
			vmName.erase(vmName.end()-1);

			// 3. obtain cpu-affinity for each virtual machine
			remoteCmd = "";
			remoteCmd = "xl vcpu-list | grep -Rw " + vmName + " | awk '{print $7}'";
			string cpu_affinity = sshCommand(hostID, remoteCmd);
			cpu_affinity.erase(cpu_affinity.end()-1); 

			// 4. obtaint locaiID for each virtual machine
			remoteCmd = "";
			remoteCmd = "xl list | grep -Rw " + vmName + " | awk '{print $2}'";
			unsigned int	localID = atoi(sshCommand(hostID, remoteCmd).c_str());

			// Create new VM
			VirtualMachine* vm;
			if ( cpu_affinity == "0-3") {
				vm = new VirtualMachine(theKey, hostID, localID, 0);
			} else {
				vm = new VirtualMachine(theKey, hostID, localID, 1);
			}

			// Register VM 
			g_vmMap.insert(pair<int, VirtualMachine*>(theKey, vm));
			g_vmNameMap.insert(pair<int, string>(theKey, vmName));
			g_hostToVM_map.insert(pair<int, VirtualMachine*>(hostID, vm));

			// 
			theKey ++ ;
		}

		cout << "Host[" << hostID << "] initialize completed.. " << endl;
	}

	// Verify
	cout << "Verify VMs" << endl;
	map<unsigned int, VirtualMachine*>::iterator it;
	VirtualMachine *vm;
	for ( it = g_vmMap.begin(); it!= g_vmMap.end(); it++ ) {

		vm = static_cast<VirtualMachine*>(it->second);
		cout << "[" << it->first << "] " << vm->getLocalID() << endl;
	}

	//
	pthread_mutex_init(&g_hostToVM_map_mutex, NULL);	

	g_highLLC_VM = new unsigned int [g_numHosts+1];
	g_lowLLC_VM = new unsigned int [g_numHosts+1];
	g_llc_mutex = new pthread_mutex_t [g_numHosts+1];

	g_hostMigrating = new bool [g_numHosts+1];
	g_hostMigrating_mutex = new pthread_mutex_t [g_numHosts+1];
	g_hostMigrating_go = new pthread_cond_t [g_numHosts+1];

	for ( unsigned int i = 0; i <= g_numHosts; i ++) {
		pthread_mutex_init(&g_llc_mutex[i], NULL);
		pthread_mutex_init(&g_hostMigrating_mutex[i], NULL);
		pthread_cond_init(&g_hostMigrating_go[i], NULL);

		g_hostMigrating[i] = false;
	}

	pthread_mutex_init(&g_migration_mutex, NULL);
	pthread_cond_init(&g_migration_done, NULL);

	return 0;
}

string sshCommand(unsigned int hostID, string command)
{
	ostringstream oss;
	string cmd;

	oss << "ssh " << g_hostPrefix << setw(2) << setfill('0') << hostID << " " << command;
	cmd = oss.str();

	//cout << cmd << endl;
		
	FILE* pipe = popen(cmd.c_str(), "r");

	if (!pipe) 
		return string("ERROR");

	char buffer[128];
	std::string result = "";

	while(!feof(pipe)) 
	{
		if(fgets(buffer, 128, pipe) != NULL) {
			// *(buffer+(strlen(buffer)-1))=0;
			result += buffer;
		}
	}

	pclose(pipe);
	return result;
}

void* migrationHelperThread(void* arg)
{
	int status;
	worker_p mine = (worker_t*)arg;
	crew_p crew = mine->crew;
	work_p	work;
	req_t	item;
	string	remoteCmd;
	stringstream hostID;

	while (! g_exitCond) {
		
		VirtualMachine* vm;
		status = pthread_mutex_lock(&crew->mutex);
		if ( status != 0 ) {
			cerr << "Lock migrationHelperThread mutex lock" << endl;
		}
		
		if (crew->first == NULL) {
			status = pthread_cond_wait(&crew->go, &crew->mutex);
			if ( status != 0 ) {
				cerr << "Wait for work in migrationHelperThread " << endl;
			}
		}
	
		work = crew->first;
		crew->first = work->next;

		if (crew->first == NULL)
			crew->last = NULL;

		memcpy(&item, &work->data, sizeof(work->data));
			
		status = pthread_mutex_unlock(&crew->mutex);
		if ( status != 0 ) {
			cerr << "Lock migrationHelperThread mutex unlock" << endl;
		}

		vm = getVM(item.vmKey);
		cout << "MigrationHelper: " << migrate( item.srcHostID, item.destHostID, vm ) << endl;

		pthread_mutex_lock(&g_migration_mutex);
		g_migrationCompleteCnt ++;
		pthread_cond_signal(&g_migration_done);
		pthread_mutex_unlock(&g_migration_mutex);

		//free(work);
		delete work;
		
		status = pthread_mutex_lock(&crew->mutex);
		crew->work_count--;
		status = pthread_mutex_unlock(&crew->mutex);

	}
	return NULL;
}

void* globalWorkerThread(void* arg)
{
	worker_p mine = (worker_t*)arg;
	crew_p crew = mine->crew;
	map<int, double>		p_missRatePerHost;	// private
	int highLLCHostID = 1, lowLLCHostID = 1;
	string	remoteCmd;
	stringstream hostID;
	req_t	work_item;
	work_p	request;
	int		status;
	unsigned int		prevMigratedHighLLC_VM = -1;
	unsigned int		prevMigratedLowLLC_VM = -1;
	int		migrationThreshold = 0;

	while (! g_exitCond) {
	
		VirtualMachine* highLLC_VM;
		VirtualMachine* lowLLC_VM;
		
		pthread_mutex_lock(&crew->mutex);
		
		if ( g_missRatePerHost.size() != g_numHosts ) {
			pthread_cond_wait(&crew->go, &crew->mutex);
		}
		cout << "Global thread wake up ! " << endl;

		p_missRatePerHost.clear();
		p_missRatePerHost.insert(g_missRatePerHost.begin(), g_missRatePerHost.end());
		g_missRatePerHost.clear();

		pthread_mutex_unlock(&crew->mutex);

		// 1. Lookup the VMs
		vector< pair<double, int> >  vt;
		vector< pair<double, int> >::iterator it_vt;
		map<int, double>::iterator it_map;

		for (it_map = p_missRatePerHost.begin(); it_map != p_missRatePerHost.end(); it_map++)
		{
			vt.push_back(make_pair(it_map->second, it_map->first));
		}

		sort(vt.rbegin(), vt.rend());

		cout << "Global LLC sorting. " << vt.size() << endl;
		for (it_vt = vt.begin(); it_vt != vt.end(); it_vt++ ) {
			cout << "Host [" << it_vt->second << "]: " << it_vt->first << endl;
		}

		highLLCHostID = vt.begin()->second;
		lowLLCHostID = vt.rbegin()->second;

		cout << "High LLC HostID [" << highLLCHostID  << "]: " << p_missRatePerHost[highLLCHostID] << endl;
		cout << "Low LLC HostID [" << lowLLCHostID  << "]: " << p_missRatePerHost[lowLLCHostID] << endl;

		if ( highLLCHostID == lowLLCHostID ) {
			goto exit;
		}

		if ( (p_missRatePerHost[highLLCHostID] - p_missRatePerHost[lowLLCHostID]) < GLOBAL_LLC_THRESHOLD ) {
			cout << "Does not meet the swap requirements" << endl;
			goto exit;
		}
		
		// 2. Get two candidate VMs
		pthread_mutex_lock(&g_llc_mutex[highLLCHostID]);
		highLLC_VM = getVM(g_highLLC_VM[highLLCHostID]);
		pthread_mutex_unlock(&g_llc_mutex[highLLCHostID]);

		pthread_mutex_lock(&g_llc_mutex[lowLLCHostID]);
		lowLLC_VM = getVM(g_lowLLC_VM[lowLLCHostID]);
		pthread_mutex_unlock(&g_llc_mutex[lowLLCHostID]);

		if ( highLLC_VM == NULL || lowLLC_VM == NULL ) {
			cout << g_highLLC_VM[highLLCHostID] << " : " << g_lowLLC_VM[lowLLCHostID] << endl;
			goto exit;
		}

		if ( highLLC_VM->getHostID() == lowLLC_VM->getHostID() ) {
			cout << "Error !!! host is same" << endl;
			goto exit;
		}

		if ( ( prevMigratedHighLLC_VM == highLLC_VM->getKey() ) && ( prevMigratedLowLLC_VM == lowLLC_VM->getKey() ) && ( migrationThreshold < 5 ) ) {
			migrationThreshold ++ ;
			cout << "VM[" << prevMigratedHighLLC_VM << "] and VM[" << prevMigratedLowLLC_VM << "] were already migrated in the last time." << endl;
			goto exit;

		}
		prevMigratedHighLLC_VM = highLLC_VM->getKey();
		prevMigratedLowLLC_VM = lowLLC_VM->getKey();
		migrationThreshold = 0;

		// 3. Swap
		cout << "Swap " << g_vmNameMap[highLLC_VM->getKey()] << "(" << highLLC_VM->getHostID() << ") and " << g_vmNameMap[lowLLC_VM->getKey()] << "(" << lowLLC_VM->getHostID() << ")" << endl;

		// 3.1 send and signal to the migration helper thread
		{
			pthread_mutex_lock(&g_migrationCrew.mutex);

			//work_item processed.
			work_item.srcHostID = highLLCHostID;
			work_item.destHostID = lowLLCHostID;
			work_item.localID = highLLC_VM->getLocalID();
			work_item.vmKey = highLLC_VM->getKey();

			request = new work_t;		
			memcpy(&request->data, &work_item, sizeof(req_t));
			request->next = NULL;

			// Adjust queue pointer
			if (g_migrationCrew.first == NULL) {
				g_migrationCrew.first = request;
				g_migrationCrew.last = request;
			} else {
				g_migrationCrew.last->next = request;
				g_migrationCrew.last = request;
			}

			g_migrationCrew.work_count++;

			// Signal to migrationCrew
			status = pthread_cond_signal (&g_migrationCrew.go);

			if (status != 0) {
				delete g_migrationCrew.first;
				g_migrationCrew.first = NULL;
				g_migrationCrew.work_count = 0;
				pthread_mutex_unlock(&g_migrationCrew.mutex);
				exit(1);
			}
			
			pthread_mutex_unlock(&g_migrationCrew.mutex);
		}

		// 3.2 
		{

			// migrate the VM
			cout << "Global: " << migrate(lowLLCHostID, highLLCHostID, lowLLC_VM) << endl;

		}
		
		// 3.3 Finalize
		{
			pthread_mutex_lock(&g_migration_mutex);
			g_migrationCompleteCnt++;

			if ( g_migrationCompleteCnt != 2 ) {
				pthread_cond_wait(&g_migration_done, &g_migration_mutex);
			}
			g_migrationCompleteCnt = 0;
			pthread_mutex_unlock(&g_migration_mutex);

			cout << "Swap completed... " << endl;
		}
exit:

		for ( unsigned int i = 0; i <= g_numHosts; i++) {
			pthread_cond_signal(&g_hostMigrating_go[i]);
		}

	}

	cout << "Global thread exit..." << endl;
	return NULL;
}

void* localWorkerThread(void *arg)
{
	worker_p mine = (worker_t*)arg;
	crew_p crew = mine->crew;
	unsigned int hostID = mine->index+1;
	printf("Crew %d starting\n", hostID);

	map<int, double>		missRatePerSocket;
	vector< pair<unsigned int, double> >	vmVector;
	vector< pair<unsigned int, double> >::iterator	vmVector_it;
	unsigned int	cpuAffinity[NUM_OF_NUMA_NODES] = {0, 1};
	int		cpuAffinityIdx = 0;
	string	remoteCmd;
	int		i = 0;
	int		numOfVMsPerSocket[NUM_OF_NUMA_NODES] = {0, 0};

	remoteCmd = "";	
	remoteCmd = "xenonmon-set.py Inst_LLC -t 7200 -n 1 ";
	cout << sshCommand(hostID, remoteCmd) << endl;

	while (! g_exitCond) {

		remoteCmd = "";
		remoteCmd = "xenonmon-do.py Inst_LLC -t 7200 -n 1 2> /dev/null";
		istringstream result(sshCommand(hostID, remoteCmd));
		string line;

		unsigned int localID;
		double numOfRetiredInsts;
		double numOfLLCMisses;
		double missRate = 0.0;
		VirtualMachine* vm;

		
		vmVector.clear();	
		missRatePerSocket.clear();
		g_missRatePerHost[hostID] = 0;
		numOfVMsPerSocket[0] = numOfVMsPerSocket[1] = 0;
		
		// For each virtual machine ( a line idicates a virtual machine )
		while (getline(result, line)) {

			// 1. Obtain # of retired insts and # of LLC misses.
			istringstream iss(line);
			
			localID = 0;
			numOfRetiredInsts = 0.0;
			numOfLLCMisses = 0.0;
			missRate = 0.0;

			iss >> localID >> numOfRetiredInsts >> numOfLLCMisses;
			// cout << "Input Stream: " << localID << "\t" << numOfRetiredInsts << "\t" << numOfLLCMisses << endl;
			if ( localID == 0 ) continue;	// Except for Domain-0
	
			map<unsigned int, VirtualMachine*>::iterator it;

			for ( it = g_vmMap.begin(); it != g_vmMap.end(); it++ ) {
				
				vm = static_cast<VirtualMachine*>(it->second);

				if ( ( vm->getHostID() == hostID ) && ( vm->getLocalID() == localID ) ) {
					
					if ( ( numOfLLCMisses < 20 ) && ( numOfRetiredInsts < 20 ) ) {
						missRate = 0.0;
					} else {
						missRate = (numOfLLCMisses * LLC_MISS_SAMPLE_THRESHOLD) / ( (numOfRetiredInsts * RETIRED_INST_SAMPLE_THRESHOLD) / 1000000);
					}

					vm->setNumRetiredInsts(numOfRetiredInsts);
					vm->setNumLLCMisses(numOfLLCMisses);
					
					if ( vm->getCPUAffinity() != getCPUAffinity(vm) ) {
						vm->setCPUAffinity(getCPUAffinity(vm));
						
						cerr << endl;
						cerr << "[" << hostID << "] Adjust " << g_vmNameMap[vm->getKey()] << " CPU affinity !!!!!!!!" << endl;
						cerr << endl;
					}

					missRatePerSocket[vm->getCPUAffinity()] += missRate;
					pthread_mutex_lock(&g_globalCrew.mutex);
					g_missRatePerHost[hostID] += missRate;
					pthread_mutex_unlock(&g_globalCrew.mutex);
					vmVector.push_back(pair<int, double>(vm->getKey(), missRate));

					numOfVMsPerSocket[vm->getCPUAffinity()] ++ ;
				}
			}
		}

		if (vmVector.size() != 8 ) {
			cout << "[" << hostID << "] Number of virtual mahcines: " << vmVector.size() << endl;
			continue;
		}

		sort(vmVector.begin(), vmVector.end(), Compare());
		
		cout << "Host [" << hostID << "] after sorting. " << vmVector.size() << endl;
		for ( vmVector_it = vmVector.begin(); vmVector_it != vmVector.end(); vmVector_it++ ) {
			cout << g_vmNameMap[static_cast<unsigned int>(vmVector_it->first)] << ": " << static_cast<double>(vmVector_it->second) << endl;
		}

		cout << "High\t" << g_vmNameMap[static_cast<unsigned int>(vmVector.begin()->first)] << ":" << vmVector.begin()->second << endl;
		cout << "Low\t" << g_vmNameMap[static_cast<unsigned int>(vmVector.rbegin()->first)] << ":" << vmVector.rbegin()->second << endl;

		// register 

		pthread_mutex_lock(&g_llc_mutex[hostID]);
		g_highLLC_VM[hostID] = vmVector.begin()->first;
		g_lowLLC_VM[hostID] = vmVector.rbegin()->first;
		pthread_mutex_unlock(&g_llc_mutex[hostID]);
		
		// Exception conditions
		vm = getVM(vmVector.begin()->first);

		if ( vm == NULL ) {
			cerr << " VM is NULL .. (1) " << endl;
			goto exit;
		} 

		if ( vm->getNumLLCMisses() < LOCAL_LLC_THRESHOLD )  {
			cout << "Does not meet the LOCAL_LLC_THRESHOLD" << endl;
			goto exit;
		}

		if ( abs(missRatePerSocket[0] - missRatePerSocket[1]) < 500 ) {
			cout << "Does not meet load unbalance" << endl;
			cout << "Socket[0-3]: " << missRatePerSocket[0] << endl;
			cout << "Socket[4-7]: " << missRatePerSocket[1] << endl;
			goto exit;
		}

		if ( vm->getCPUAffinity() != cpuAffinity[cpuAffinityIdx] ) {
			cpuAffinityIdx = !cpuAffinityIdx;
		}

		cout << "Host [" << hostID << "] Changing CPU-AFFINITY " << endl;
		for ( vmVector_it = vmVector.begin(); vmVector_it != vmVector.end(); vmVector_it++, i++) {

			vm = getVM(vmVector_it->first);
			cout << "[" << hostID << "] " << g_vmNameMap[vm->getKey()] << " [" << vm->getLocalID() << "]\t CPU-affinity: " << cpuAffinity[cpuAffinityIdx] << endl;
			
			if ( cpuAffinity[cpuAffinityIdx] != vm->getCPUAffinity() ) {

				setCPUAffinity(cpuAffinity[cpuAffinityIdx], vm);
			}

			if ( i != 3 ) {
				cpuAffinityIdx = !cpuAffinityIdx;
			}
		}

exit:
		pthread_barrier_wait(&crew->barrier);
		pthread_cond_signal(&g_globalCrew.go);

		pthread_mutex_lock(&g_hostMigrating_mutex[hostID]);
		pthread_cond_wait(&g_hostMigrating_go[hostID], &g_hostMigrating_mutex[hostID]);
		pthread_mutex_unlock(&g_hostMigrating_mutex[hostID]);

		sleep(LOCAL_SCHD_TIME_INTERVAL);
	}
	
	remoteCmd = "";
	remoteCmd = "xenonmon-unset.py Inst_LLC -t 7200 -n 1";
	cout << sshCommand(hostID, remoteCmd) << endl;
	cout << "Host [" << hostID << "] thread exit..." << endl;

	return NULL;
}

numaMemoryInfo getNUMAAffinity(int hostID, int localID)
{
	numaMemoryInfo memInfo;
	string remoteCmd;
	stringstream ss_localID;
	istringstream iss;
	string line;

	ss_localID.str("");
	ss_localID << localID;

	remoteCmd = "";
	remoteCmd = "./getNUMA-affinity.sh " + ss_localID.str();
	istringstream cmdResult(sshCommand(hostID, remoteCmd));
	
	getline(cmdResult, line);
	iss.str(line);
	iss >> memInfo.numOfPages[0] >> memInfo.numOfPages[1];

	return memInfo;
}


VirtualMachine* getVM(unsigned int key)
{
	map<unsigned int, VirtualMachine*>::iterator it;
	it = g_vmMap.find(key);

	if ( it == g_vmMap.end() ) {
		return NULL;
	} else {
		return it->second;
	}
}

unsigned int getCPUAffinity(VirtualMachine* vm)
{
	string remoteCmd, cpu_affinity;
	remoteCmd = "";
	remoteCmd = "xm vcpu-list | grep -Rw " + g_vmNameMap[vm->getKey()] + " | awk '{print $7}'";
	cpu_affinity = sshCommand(vm->getHostID(), remoteCmd);

	if ( ( cpu_affinity != "0-3\n" ) && ( cpu_affinity != "4-7\n") ) {
		cout << "[" << vm->getHostID() << "] Req: " << remoteCmd << endl;
		cout << "[" << vm->getHostID() << "] Res: " << cpu_affinity << endl;

		//return 3;
	}
	cpu_affinity.erase(cpu_affinity.end()-1); 

	if ( cpu_affinity == "0-3" ) {
		return 0;
	} else if ( cpu_affinity == "4-7") {
		return 1;
	}

	return -1;
}

unsigned int getLocalID(VirtualMachine* vm)
{
	string remoteCmd;

	remoteCmd = "";
	remoteCmd = "xm list | grep -Rw " + g_vmNameMap[vm->getKey()] + " | awk '{print $2}'";
	return atoi(sshCommand(vm->getHostID(), remoteCmd).c_str());
}

string migrate(int srcHostID, int destHostID, VirtualMachine* vm, int node)
{
	stringstream hostID;
	string remoteCmd;
	
	hostID.str("");
	hostID << setw(2) << setfill('0') << destHostID;

	remoteCmd = "";
	if ( node == 1) {
		remoteCmd = "xm migrate -l -n 1 " + g_vmNameMap[vm->getKey()] + " " + g_hostPrefix + hostID.str();
	} else {
		remoteCmd = "xm migrate -l " + g_vmNameMap[vm->getKey()] + " " + g_hostPrefix + hostID.str();
	}
	sshCommand(srcHostID, remoteCmd); 

	vm->setHostID(destHostID);
	vm->setLocalID( getLocalID(vm) );

	return remoteCmd;
}

string setCPUAffinity( int affinity, VirtualMachine* vm)
{
	string remoteCmd;
	remoteCmd = "";
	
	// must use xm insted of xl
	if ( affinity == 0 ) {
		remoteCmd = "xm vcpu-pin " + g_vmNameMap[vm->getKey()] + " 0 0-3";
	} else {
		remoteCmd = "xm vcpu-pin " + g_vmNameMap[vm->getKey()] + " 0 4-7";
	}

	sshCommand(vm->getHostID(), remoteCmd); 
	vm->setCPUAffinity(affinity);

	return remoteCmd;
}

