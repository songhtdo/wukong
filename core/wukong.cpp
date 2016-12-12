/*
 * Copyright (c) 2016 Shanghai Jiao Tong University.
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://ipads.se.sjtu.edu.cn/projects/wukong.html
 *
 */

#include <boost/mpi.hpp>
#include <boost/serialization/string.hpp>
#include <iostream>

#include "config.hpp"
#include "string_server.hpp"
#include "dgraph.hpp"
#include "engine.hpp"
#include "proxy.hpp"
#include "console.hpp"
#include "monitor.hpp"

#include "unit.hpp"

using namespace std;

/*
 * The processor architecture of our cluster (Cube0-5)
 *
 * $numactl --hardware
 * available: 2 nodes (0-1)
 * node 0 cpus: 0 2 4 6 8 10 12 14 16 18 20 22 24 26 28 30 32 34 36 38
 * node 0 size: 64265 MB
 * node 0 free: 19744 MB
 * node 1 cpus: 1 3 5 7 9 11 13 15 17 19 21 23 25 27 29 31 33 35 37 39
 * node 1 size: 64503 MB
 * node 1 free: 53586 MB
 * node distances:
 * node   0   1
 *   0:  10  21
 *   1:  21  10
 *
 * TODO:
 * co-locate proxy and engine threads to the same socket,
 * and bind them to the same socket. For example, 2 proxy thread with
 * 8 engine threads for each 10-core processor.
 *
 */
int cores[] = {
	1, 3, 5, 7, 9, 11, 13, 15, 17, 19,
	0, 2, 4, 6, 8, 10, 12, 14, 16, 18
};

bool monitor_enable = false;
int monitor_port = 5450;

void pin_to_core(size_t core)
{
	cpu_set_t  mask;
	CPU_ZERO(&mask);
	CPU_SET(core, &mask);
	int result = sched_setaffinity(0, sizeof(mask), &mask);
}

void *engine_thread(void *arg)
{
	Engine *engine = (Engine *)arg;
	pin_to_core(cores[engine->tid]);
	engine->run();
}

void *proxy_thread(void *arg)
{
	Proxy *proxy = (Proxy *)arg;
	pin_to_core(cores[proxy->tid]);
	if (!monitor_enable)
		// Run the Wukong's testbed console (by default)
		run_console(proxy);
	else
		// Run monitor thread for clients
		run_monitor(proxy, monitor_port);
}

static void
usage(char *fn)
{
	cout << "usage: << fn <<  <config_fname> <host_fname> [options]" << endl;
	cout << "options:" << endl;
	cout << "  -c: enable connected client" << endl;
	cout << "  -p port_num : the port number of connected client (default: 5450)" << endl;
}

int
main(int argc, char *argv[])
{
	boost::mpi::environment env(argc, argv);
	boost::mpi::communicator world;
	int sid = world.rank(); // server ID

	if (argc < 3) {
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	cfg_fname = std::string(argv[1]);
	host_fname = std::string(argv[2]);

	int c;
	while ((c = getopt(argc - 2, argv + 2, "cp:")) != -1) {
		switch (c) {
		case 'c':
			monitor_enable = true;
			break;
		case 'p':
			monitor_port = atoi(optarg);
			break;
		default :
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	// load global configuration setting
	load_config(world.size());

	// calculate memory usage
	uint64_t rdma_size = GiB2B(global_total_memory_gb);
	uint64_t msg_slot_per_thread = MiB2B(global_perslot_msg_mb);
	uint64_t rdma_slot_per_thread = MiB2B(global_perslot_rdma_mb);

	// RDMA = buffer + msgslot + rdmaslot
	uint64_t mem_size = rdma_size
	                    + rdma_slot_per_thread * global_num_threads
	                    + msg_slot_per_thread * global_num_threads;
	cout << "INFO: RDMA memory usage (per server): " << B2GiB(mem_size) << "GB" << endl;

	// create an RDMA instance
	char *buffer = (char *)malloc(mem_size);
	memset(buffer, 0, mem_size);
	RdmaResource *rdma = new RdmaResource(global_num_servers, global_num_threads,
	                                      sid, host_fname, buffer, mem_size,
	                                      rdma_slot_per_thread, msg_slot_per_thread, rdma_size);
#ifdef HAS_RDMA
	rdma->servicing();
	rdma->connect();
#endif

	// load string server (read-only, shared by all proxies)
	String_Server str_server(global_input_folder);

	// load RDF graph (shared by all engines)
	DGraph dgraph(sid, rdma, global_input_folder);

	// initiate proxy and engine threads
	assert(global_num_threads == global_num_proxies + global_num_engines);
	pthread_t *threads  = new pthread_t[global_num_threads];
	for (int tid = 0; tid < global_num_threads; tid++) {
		/// TODO: currently, rdma is shared by all threads,
		/// therefore, we create tcp in here not within adaptor
		TCP_Adaptor *tcp = new TCP_Adaptor(sid, tid, host_fname);
		if (tid < global_num_proxies) {
			Proxy *proxy = new Proxy(sid, tid, &str_server, tcp, rdma);
			pthread_create(&(threads[tid]), NULL, proxy_thread, (void *)proxy);
			proxies.push_back(proxy);
		} else {
			Engine *engine = new Engine(sid, tid, &dgraph, tcp, rdma);
			pthread_create(&(threads[tid]), NULL, engine_thread, (void *)engine);
			engines.push_back(engine);
		}
	}

	// wait to all threads termination
	for (size_t t = 0; t < global_num_threads; t++) {
		int rc = pthread_join(threads[t], NULL);
		if (rc) {
			printf("ERROR: return code from pthread_join() is %d\n", rc);
			exit(-1);
		}
	}

	/// TODO: exit gracefully (properly call MPI_Init() and MPI_Finalize(), delete all objects)
	delete rdma;

	return 0;
}

