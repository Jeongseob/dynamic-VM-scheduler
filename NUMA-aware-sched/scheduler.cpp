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

#include <float.h>
#include <time.h>
#include "virtualMachine.h"
#include "crew.h"

#define LLC_MISS_SAMPLE_THRESHOLD           10000
#define RETIRED_INST_SAMPLE_THRESHOLD       500000

#define LOCAL_LLC_THRESHOLD					50
#define GLOBAL_LLC_THRESHOLD				1000
#define	NUMA_THRESHOLD						2000 // 10000

#define LOCAL_SCHD_TIME_INTERVAL			5	// 10
#define GLOBAL_SCHD_TIME_INTERVAL			15
#define	NUM_OF_NUMA_NODES					2
#define DEGREE_OF_MIGRATION					4

using namespace std;

typedef pair<int , int > virtualMachineKey;
typedef pair<int , int > socketKey;

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
map<socketKey, double>				g_missRatePerSocket;

unsigned int**		g_highLLC_VM;
unsigned int**		g_lowLLC_VM;
pthread_mutex_t**	g_llc_mutex;

bool*				g_hostMigrating;
pthread_cond_t*		g_hostMigrating_go;
pthread_mutex_t*	g_hostMigrating_mutex;

static crew_t		g_localCrew;
static crew_t		g_globalCrew;
static crew_t		g_migrationCrew;

pthread_mutex_t		g_migration_mutex;
pthread_cond_t		g_migration_done;
int					g_migrationCompleteCnt = 0;
int					g_migrationReqCnt = 0;

string	g_hostPrefix;
bool g_exitCond = false;
unsigned int g_numHosts = 0;
int g_degreeOfMigration = DEGREE_OF_MIGRATION;

int main(int argc, char *argv[])
{
	int status;
	
	// Default number of workers 1
	g_numHosts = CREW_SIZE;
		
	if (argc < 4) {
		cerr << "usage: " << argv[0] << " [host_prefix] [number of hosts] [degree of migration]" << endl;
		exit(1);
	}

	g_hostPrefix = argv[1];
	g_numHosts = atoi(argv[2]);
	g_degreeOfMigration = atoi(argv[3]);

	cout << "Host prefix: " << g_hostPrefix << endl;
	cout << "Num of hosts: " << g_numHosts << endl;
	cout << "Degree of migration: " << g_degreeOfMigration << endl;

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
	status = create_crew(&g_migrationCrew, g_degreeOfMigration*2, migrationHelperThread);
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

	g_highLLC_VM = new unsigned int* [g_numHosts+1];
	g_lowLLC_VM = new unsigned int* [g_numHosts+1];
	g_llc_mutex = new pthread_mutex_t* [g_numHosts+1];

	for ( unsigned int i = 0; i < g_numHosts+1; i++ ) {
		g_highLLC_VM[i] = new unsigned int [NUM_OF_NUMA_NODES];
		g_lowLLC_VM[i] = new unsigned int [NUM_OF_NUMA_NODES];
		g_llc_mutex[i] = new pthread_mutex_t [NUM_OF_NUMA_NODES];
	}

	for ( unsigned int i = 0; i < g_numHosts+1; i++ ) {

		for ( unsigned int j = 0; j < NUM_OF_NUMA_NODES; j++) {
			pthread_mutex_init(&g_llc_mutex[i][j], NULL);
		}
	}

	g_hostMigrating = new bool [g_numHosts+1];
	g_hostMigrating_mutex = new pthread_mutex_t [g_numHosts+1];
	g_hostMigrating_go = new pthread_cond_t [g_numHosts+1];

	for ( unsigned int i = 0; i <= g_numHosts; i ++) {
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

	pthread_mutex_lock(&crew->mutex);

	/*
	 *	when crews are created, work queue is empty.
	 *	so, crew wait until anyone put job into queue
	 */
	while (crew->work_count == 0) {
		pthread_cond_wait(&crew->go, &crew->mutex);
	}
	pthread_mutex_unlock(&crew->mutex);

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
		
		if ( vm->getCPUAffinity() != item.adversaryVmAffinity )	{
	
			cout << "[" << item.srcHostID << "] MigrationHelper: " << setCPUAffinity(item.adversaryVmAffinity, vm) << endl;
		}

		cout << "MigrationHelper: " << migrate( item.srcHostID, item.destHostID, vm ) << endl;

		pthread_mutex_lock(&g_migration_mutex);
		g_migrationCompleteCnt ++;
		if ( g_migrationCompleteCnt == (g_migrationReqCnt * 2) ) {
			pthread_cond_signal(&g_migration_done);
		}
		pthread_mutex_unlock(&g_migration_mutex);

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
	unsigned int id = mine->index;
	map<socketKey, double>		p_missRatePerSocket;	// private
	socketKey highLLCSocketID[g_degreeOfMigration], lowLLCSocketID[g_degreeOfMigration];
	string	remoteCmd;
	stringstream hostID;
	req_t	work_item;
	work_p	request;
	int		status;
	unsigned int	prevMigratedHighLLC_VM[g_degreeOfMigration];
	unsigned int	prevMigratedLowLLC_VM[g_degreeOfMigration];
	int				migrationThreshold[g_degreeOfMigration];
	int		cnt = 0;
	bool	migrationReq[g_degreeOfMigration];

	for ( int i = 0; i < g_degreeOfMigration; i ++ ) {
		prevMigratedLowLLC_VM[i] = -1;
		prevMigratedHighLLC_VM[i] = -1;
		migrationThreshold[i] = 0;
	}

	while (! g_exitCond) {
		
	
		VirtualMachine* highLLC_VM[g_degreeOfMigration];
		VirtualMachine* lowLLC_VM[g_degreeOfMigration];
		unsigned int	lowLLC_VM_affinity[g_degreeOfMigration];
		unsigned int 	highLLC_VM_affinity[g_degreeOfMigration];
		
		pthread_mutex_lock(&crew->mutex);
		
		if ( g_missRatePerSocket.size() != (g_numHosts * NUM_OF_NUMA_NODES) ) {
			pthread_cond_wait(&crew->go, &crew->mutex);
		}
		cout << "[" << id << "] Global thread wake up ! " << endl;

		p_missRatePerSocket.clear();
		p_missRatePerSocket.insert(g_missRatePerSocket.begin(), g_missRatePerSocket.end());
		g_missRatePerSocket.clear();

		pthread_mutex_unlock(&crew->mutex);

		// 1. Lookup the VMs
		vector< pair<double, socketKey > >  vt;
		vector< pair<double, socketKey > >::iterator it_vt;
		vector< pair<double, socketKey > >::reverse_iterator rit_vt;
		map<socketKey, double>::iterator it_map;

		/*
		for (it_map = p_missRatePerSocket.begin(); it_map != p_missRatePerSocket.end(); it_map++)
		{
			vt.push_back(make_pair(it_map->second, it_map->first));
		}

		sort(vt.rbegin(), vt.rend());

		cout << "[" << id << "] Global LLC sorting. " << vt.size() << endl;
		for (it_vt = vt.begin(); it_vt != vt.end(); it_vt++ ) {
			socketKey key = static_cast<socketKey>(it_vt->second);
			cout << "Socket [" << key.first << "][" << key.second << "]: " << it_vt->first << endl;
		}
		*/

		/*
		it_vt = vt.begin();
		rit_vt = vt.rbegin();
		highLLCSocketID[0] = it_vt->second;
		lowLLCSocketID[0] = rit_vt->second;

		// FIX: must avoid the hard coded parts
		cnt = 0;
		do { 
			if ( cnt > g_numHosts || cnt >= vt.size() ) {
				cout << "high- migrationReq[1] false: " << cnt << endl;
				migrationReq[1] = false;
				break;
			}
			highLLCSocketID[1] = (++it_vt)->second;
			cnt ++; 
		} while ( highLLCSocketID[1].first == highLLCSocketID[0].first || highLLCSocketID[1].first == lowLLCSocketID[0].first ); 
		
		cnt = 0;
		do { 
			if ( cnt > g_numHosts || cnt >= vt.size()  ) {
				cout << "low- migrationReq[1] false: " <<  cnt <<  endl;
				migrationReq[1] = false;
				break;
			}
			lowLLCSocketID[1] = (++rit_vt)->second;
			cnt ++;
		} while ( lowLLCSocketID[1].first == lowLLCSocketID[0].first || lowLLCSocketID[1].first == highLLCSocketID[0].first ); 
		*/

		//
		for ( int i = 0; i < g_degreeOfMigration; i ++ ) {

			highLLCSocketID[i].first = -1;
			highLLCSocketID[i].second = -1;
			lowLLCSocketID[i].first = -1;
			lowLLCSocketID[i].second = -1;

			vt.clear();
			for (it_map = p_missRatePerSocket.begin(); it_map != p_missRatePerSocket.end(); it_map++)
			{

				socketKey key = it_map->first;
				bool	insert = true;

				for (int j = 0; j < i+1; j ++ ) {
					if ( key.first == highLLCSocketID[j].first  || key.first == lowLLCSocketID[j].first ) {
						insert = false;
					}
				}
						
				if ( insert ) {
					vt.push_back(make_pair(it_map->second, it_map->first));
				}

			}

			sort(vt.rbegin(), vt.rend());
			
			cout << "[" << i << "] Global LLC sorting. " << vt.size() << endl;
			for (it_vt = vt.begin(); it_vt != vt.end(); it_vt++ ) {
				socketKey key = static_cast<socketKey>(it_vt->second);
				cout << "Socket [" << key.first << "][" << key.second << "]: " << it_vt->first << endl;
			}


			it_vt = vt.begin();
			rit_vt = vt.rbegin();
			highLLCSocketID[i] = it_vt->second;
			lowLLCSocketID[i] = rit_vt->second;
		}

	
		//

		/*
		cnt = 0;
		for (it_vt = vt.begin(); it_vt != vt.end(); it_vt++ ) {

			if ( cnt >= g_degreeOfMigration ) break;

			highLLCSocketID[cnt++] = it_vt->second;
		}

		cnt = 0;
		for (rit_vt = vt.rbegin(); rit_vt != vt.rend(); rit_vt++ ) {

			if ( cnt >= g_degreeOfMigration ) break;

			lowLLCSocketID[cnt++] = rit_vt->second;
		}
		*/

		for ( int i = 0 ; i < g_degreeOfMigration; i++) {
			cout << i << ". High LLC SocketID [" << highLLCSocketID[i].first  << "][" << highLLCSocketID[i].second << "]: " << p_missRatePerSocket[highLLCSocketID[i]] << endl;
			cout << i << ". Low LLC SocketID [" << lowLLCSocketID[i].first  << "][" << lowLLCSocketID[i].second << "]: " << p_missRatePerSocket[lowLLCSocketID[i]] << endl;
		}

		/////
		
		for ( int i = 0 ; i < g_degreeOfMigration; i++) {

			if ( highLLCSocketID[i].first == lowLLCSocketID[i].first) {

				if ( highLLCSocketID[i].second == lowLLCSocketID[i].second ) {
					//goto exit;
					cerr << "SocketID same !! " << endl;
					migrationReq[i] = false;
				}
			}

			if ( (p_missRatePerSocket[highLLCSocketID[i]] - p_missRatePerSocket[lowLLCSocketID[i]]) < GLOBAL_LLC_THRESHOLD ) {
				cout << "Does not meet the swap requirements" << endl;
				migrationReq[i] = false;
				//goto exit;
			}
		}
		
		// 2. Get two candidate VMs
		for ( int i = 0; i < g_degreeOfMigration; i++ ) {

			if ( migrationReq[i] == true  ) {
				pthread_mutex_lock(&g_llc_mutex[highLLCSocketID[i].first][highLLCSocketID[i].second]);
				highLLC_VM[i] = getVM(g_highLLC_VM[highLLCSocketID[i].first][highLLCSocketID[i].second]);
				pthread_mutex_unlock(&g_llc_mutex[highLLCSocketID[i].first][highLLCSocketID[i].second]);

				pthread_mutex_lock(&g_llc_mutex[lowLLCSocketID[i].first][lowLLCSocketID[i].second]);
				lowLLC_VM[i] = getVM(g_lowLLC_VM[lowLLCSocketID[i].first][lowLLCSocketID[i].second]);
				pthread_mutex_unlock(&g_llc_mutex[lowLLCSocketID[i].first][lowLLCSocketID[i].second]);
			}

		}

		for ( int i = 0 ; i < g_degreeOfMigration; i++) {

			if ( highLLC_VM[i] == NULL || lowLLC_VM[i] == NULL ) {
				cout << i << " " << g_highLLC_VM[highLLCSocketID[i].first][highLLCSocketID[i].second] << " : " << g_lowLLC_VM[lowLLCSocketID[i].first][lowLLCSocketID[i].second] << endl;
				migrationReq[i] = false;
				//goto exit;
			}
			
			if ( migrationReq[i] == true ) {			
				if ( ( prevMigratedHighLLC_VM[i] == highLLC_VM[i]->getKey() ) && ( prevMigratedLowLLC_VM[i] == lowLLC_VM[i]->getKey() ) && ( migrationThreshold[i] < 5 ) ) {
					migrationThreshold[i] ++ ;
					cout << "VM[" << prevMigratedHighLLC_VM[i] << "] and VM[" << prevMigratedLowLLC_VM[i] << "] were already migrated in the last time." << endl;
					migrationReq[i] = false;
					//goto exit;
				}
			}
			

			if ( migrationReq[i] == true ) {
				prevMigratedHighLLC_VM[i] = highLLC_VM[i]->getKey();
				prevMigratedLowLLC_VM[i] = lowLLC_VM[i]->getKey();
				migrationThreshold[i] = 0;
			}

			if ( migrationReq[i] == true ) {
				highLLC_VM_affinity[i] = highLLC_VM[i]->getCPUAffinity();
				lowLLC_VM_affinity[i] = lowLLC_VM[i]->getCPUAffinity();
			}
		}

		// 3. Swap
		for ( int i = 0 ; i < g_degreeOfMigration; i++) {
			if ( migrationReq[i] == true )  {
				cout << "[" << id << "] Swap " << g_vmNameMap[highLLC_VM[i]->getKey()] << "(" << highLLC_VM[i]->getHostID() << ") and " << g_vmNameMap[lowLLC_VM[i]->getKey()] << "(" << lowLLC_VM[i]->getHostID() << ")" << endl;
				pthread_mutex_lock(&g_migration_mutex);
				g_migrationReqCnt++;
				pthread_mutex_unlock(&g_migration_mutex);
			}
		}

		// 3.1 send and signal to the migration helper thread
		{
			for ( int i = 0 ; i < g_degreeOfMigration; i++) {
				
				if ( migrationReq[i] != true ) continue;

				pthread_mutex_lock(&g_migrationCrew.mutex);

				//work_item processed.
				work_item.srcHostID = highLLCSocketID[i].first;
				work_item.destHostID = lowLLCSocketID[i].first;
				work_item.localID = highLLC_VM[i]->getLocalID();
				work_item.vmKey = highLLC_VM[i]->getKey();
				work_item.adversaryVmAffinity = lowLLC_VM_affinity[i];

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

				///////////////////////////////////////////////////
				//work_item processed.
				work_item.srcHostID = lowLLCSocketID[i].first;
				work_item.destHostID = highLLCSocketID[i].first;
				work_item.localID = lowLLC_VM[i]->getLocalID();
				work_item.vmKey = lowLLC_VM[i]->getKey();
				work_item.adversaryVmAffinity = highLLC_VM_affinity[i];

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
		}

		// 3.3 Finalize
		{
			pthread_mutex_lock(&g_migration_mutex);
			if ( g_migrationCompleteCnt != (g_migrationReqCnt * 2) ) {
				pthread_cond_wait(&g_migration_done, &g_migration_mutex);
			}
			g_migrationCompleteCnt = 0;
			g_migrationReqCnt = 0;
			pthread_mutex_unlock(&g_migration_mutex);

			cout << "Swap completed... " << endl;
		}


		/*
		// 3.2 
		{
			// migrate the VM

			if ( lowLLC_VM->getCPUAffinity() != highLLC_VM_affinity ) 	{
			
				cout << "[" << lowLLCSocketID.first << "][" << lowLLCSocketID.second << "] Global: " << setCPUAffinity( highLLC_VM_affinity, lowLLC_VM) << endl;
			}

			cout << "[" << id << "] Global: " << migrate(lowLLCSocketID.first, highLLCSocketID.first, lowLLC_VM) << endl;

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
		*/
exit:
		sleep(GLOBAL_SCHD_TIME_INTERVAL);
		
		for ( unsigned int i = 0; i <= g_numHosts; i++) {
			pthread_cond_signal(&g_hostMigrating_go[i]);
		}

		for ( int i = 0; i < g_degreeOfMigration; i ++ ) {
			migrationReq[i] = true;
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

	map<int, double>		vmMapPerHost;
	map<int, double>::iterator it_vmMap;
	map<int, double>		missRatePerSocket;
	vector< pair<unsigned int, double> >	vmVector[NUM_OF_NUMA_NODES];
	vector< pair<unsigned int, double> >::iterator	vmVector_it;
	string	remoteCmd;
	int		numaInterval = 1;
	int		resetCounter = 1;
	int		numOfVMsPerSocket[NUM_OF_NUMA_NODES] = {0, 0};

	remoteCmd = "";	
	remoteCmd = "xenonmon-set.py Inst_LLC -t 7200 -n 1 ";
	cout << sshCommand(hostID, remoteCmd) << endl;

	while (! g_exitCond) {

		remoteCmd = "";
		remoteCmd = "xenonmon-do.py Inst_LLC -t 7200 -n 1 2> /dev/null";
		istringstream result(sshCommand(hostID, remoteCmd));
		string line;

		pair<int, int> socketKey;
		unsigned int localID;
		double numOfRetiredInsts;
		double numOfLLCMisses;
		double missRate = 0.0;
		VirtualMachine* vm;

		vmVector[0].clear();	
		vmVector[1].clear();
		numOfVMsPerSocket[0] = numOfVMsPerSocket[1] = 0;

		for ( int i = 0 ; i < NUM_OF_NUMA_NODES; i ++ ) {
			socketKey = make_pair(hostID, i);
			g_missRatePerSocket[socketKey] = 0;
		}
	
		missRatePerSocket.clear();
		vmMapPerHost.clear();
	
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

					/*
					if ( vm->getCPUAffinity() != getCPUAffinity(vm) ) {
						vm->setCPUAffinity(getCPUAffinity(vm));
						
						cerr << endl;
						cerr << "[" << hostID << "] Adjust " << g_vmNameMap[vm->getKey()] << " CPU affinity !!!!!!!!" << endl;
						cerr << endl;
					}
					*/

					missRatePerSocket[vm->getCPUAffinity()] += missRate;
					pthread_mutex_lock(&g_globalCrew.mutex);
					socketKey = make_pair(hostID, vm->getCPUAffinity());
					g_missRatePerSocket[socketKey] += missRate;
					pthread_mutex_unlock(&g_globalCrew.mutex);
					vmVector[vm->getCPUAffinity()].push_back(pair<int, double>(vm->getKey(), missRate));

					numOfVMsPerSocket[vm->getCPUAffinity()] ++ ;
				}
			}
		}

		if (vmVector[0].size() != 4 || vmVector[1].size() !=4 ) {
			cout << "[" << hostID << "][0] Number of virtual mahcines: " << vmVector[0].size() << endl;
			cout << "[" << hostID << "][1] Number of virtual mahcines: " << vmVector[1].size() << endl;
			vmMapPerHost.clear();
			continue;
		}

		sort(vmVector[0].begin(), vmVector[0].end(), Compare());
		sort(vmVector[1].begin(), vmVector[1].end(), Compare());
		
		cout << "Host [" << hostID << "] after sorting. " << endl;
		for ( int i = 0; i < NUM_OF_NUMA_NODES; i ++ ) {
			for ( vmVector_it = vmVector[i].begin(); vmVector_it != vmVector[i].end(); vmVector_it++ ) {
				cout << g_vmNameMap[static_cast<unsigned int>(vmVector_it->first)] << ": " << static_cast<double>(vmVector_it->second) << endl;
			}
		}

		for ( int i = 0; i < NUM_OF_NUMA_NODES; i ++ ) {
			cout << "[" << i << "]High\t" << g_vmNameMap[static_cast<unsigned int>(vmVector[i].begin()->first)] << ":" << vmVector[i].begin()->second << endl;
			cout << "[" << i << "]Low\t" << g_vmNameMap[static_cast<unsigned int>(vmVector[i].rbegin()->first)] << ":" << vmVector[i].rbegin()->second << endl;
		}

		// register 

		for ( int i = 0; i < NUM_OF_NUMA_NODES; i ++ ) {
			pthread_mutex_lock(&g_llc_mutex[hostID][i]);
			g_highLLC_VM[hostID][i] = vmVector[i].begin()->first;
			g_lowLLC_VM[hostID][i] = vmVector[i].rbegin()->first;
			pthread_mutex_unlock(&g_llc_mutex[hostID][i]);
		}
		
		if ( numaInterval % 5 == 0 ) {

			for ( int i = 0; i < NUM_OF_NUMA_NODES; i ++ ) {

				for ( vmVector_it = vmVector[i].begin(); vmVector_it != vmVector[i].end(); vmVector_it++) {

					if ( vmVector_it->second > NUMA_THRESHOLD ) {

						vm = getVM(g_highLLC_VM[hostID][i]);
						numaMemoryInfo memInfo = getNUMAAffinity(hostID, vm->getLocalID());

						if ( ( vm->getCPUAffinity() == 0 ) && ( memInfo.numOfPages[0] != 262144 ) ) {

							cout << "[" << hostID << "] NUMA migration: " << migrate(hostID, hostID, vm) << endl;
							break;

						} else if ( ( vm->getCPUAffinity() == 1 ) && ( memInfo.numOfPages[1] != 262144 ) ) {

							cout << "[" << hostID << "] NUMA migration: " << migrate(hostID, hostID, vm) << endl;
							break;
						}
					}
				}
			}

			numaInterval = 1;
		}

		numaInterval ++;
		resetCounter ++ ;
exit:
		pthread_barrier_wait(&crew->barrier);
		pthread_cond_signal(&g_globalCrew.go);

		pthread_mutex_lock(&g_hostMigrating_mutex[hostID]);
		pthread_cond_wait(&g_hostMigrating_go[hostID], &g_hostMigrating_mutex[hostID]);
		pthread_mutex_unlock(&g_hostMigrating_mutex[hostID]);

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

