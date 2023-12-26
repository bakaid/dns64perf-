/* dns64perf++ - C++14 DNS64 performance tester
 * Based on dns64perf by Gabor Lencse <lencse@sze.hu>
 * (http://ipv6.tilb.sze.hu/dns64perf/)
 * Copyright (C) 2017  Daniel Bakai <bakaid@kszk.bme.hu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#include "dnstester.h"
#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>

int main(int argc, char *argv[]) {
#ifdef DNS64PERFPP_IPV4
  struct in_addr server_addr;
#else
  struct in6_addr server_addr;
#endif
  uint16_t port;
  uint32_t ip;
  uint8_t netmask;
  uint32_t num_req, num_burst, num_thread;
  uint16_t num_port;
  uint64_t burst_delay;
  struct timeval timeout;
  if (argc < 10) {
    std::cerr << "Usage: dns64perf++ <server> <port> <subnet> <number of "
                 "requests> <burst size> <number of threads> <number of ports "
                 "per thread> <delay between "
                 "bursts in ns> <timeout in s>"
              << std::endl;
    return -1;
  }
/* Server address */
#ifdef DNS64PERFPP_IPV4
  if (inet_pton(AF_INET, argv[1], reinterpret_cast<void *>(&server_addr)) !=
      1) {
#else
  if (inet_pton(AF_INET6, argv[1], reinterpret_cast<void *>(&server_addr)) !=
      1) {
#endif
    std::cerr << "Bad server adddress." << std::endl;
    return -1;
  }
  /* Port */
  if (sscanf(argv[2], "%hu", &port) != 1) {
    std::cerr << "Bad port." << std::endl;
    return -1;
  }
  /* Subnet */
  uint8_t temp[4];
  if (sscanf(argv[3], "%hhu.%hhu.%hhu.%hhu/%hhu", temp, temp + 1, temp + 2,
             temp + 3, &netmask) != 5) {
    std::cerr << "Bad subnet." << std::endl;
    return -1;
  }
  if (netmask > 32) {
    std::cerr << "Bad netmask." << std::endl;
    return -1;
  }
  ip = ((temp[0] << 24) | (temp[1] << 16) | (temp[2] << 8) | temp[3]) &
       ~(((uint64_t)1 << (32 - netmask)) - 1);
  /* Number of requests */
  if (sscanf(argv[4], "%u", &num_req) != 1) {
    std::cerr << "Bad number of requests, must be between 0 and 2^32."
              << std::endl;
    return -1;
  }
  if (num_req > ((uint64_t)1 << (32 - netmask))) {
    std::cerr << "The number of requests is higher than the avaliable IPs in "
                 "the subnet."
              << std::endl;
    return -1;
  }
  /* Burst size */
  if (sscanf(argv[5], "%u", &num_burst) != 1) {
    std::cerr << "Bad burst size, must be between 0 and 2^32." << std::endl;
    return -1;
  }
  /* Number of threads */
  if (sscanf(argv[6], "%u", &num_thread) != 1) {
    std::cerr << "Bad number of threads size, must be between 0 and 2^32."
              << std::endl;
    return -1;
  }
  /* Sanity check */
  if (num_req % (num_thread * num_burst) != 0) {
    std::cerr << "Number of requests must be divisble by (number of threads * "
                 "burst size)"
              << std::endl;
    return -1;
  }
  /* Number of ports per thread */
  if (sscanf(argv[7], "%hu", &num_port) != 1) {
    std::cerr << "Bad number of ports per thread, must be between 0 and 2^16."
              << std::endl;
    return -1;
  }
  /* Burst delay */
  if (sscanf(argv[8], "%lu", &burst_delay) != 1) {
    std::cerr << "Bad delay between bursts." << std::endl;
    return -1;
  }
  /* Timeout */
  double timeout_, s, us;
  if (sscanf(argv[9], "%lf", &timeout_) != 1) {
    std::cerr << "Bad timeout." << std::endl;
    return -1;
  }
  us = modf(timeout_, &s) * 1000000;
  timeout.tv_sec = (time_t)s;
  timeout.tv_usec = (suseconds_t)us;

  std::vector<std::unique_ptr<DnsTester>> testers;
  std::vector<std::thread> threads;
  auto reference_time =
      std::chrono::high_resolution_clock::now() + std::chrono::seconds(2);
  for (uint32_t i = 0; i < num_thread; i++) {
    testers.emplace_back(std::make_unique<DnsTester>(
        server_addr, port, ip, netmask, num_req, num_burst, num_thread, i,
        num_port,
        reference_time + std::chrono::nanoseconds{burst_delay / num_thread} * i,
        std::chrono::nanoseconds{burst_delay}, timeout));
  }
  try {
    for (uint32_t i = 0; i < num_thread; i++) {
      threads.emplace_back([&, i]() { testers[i]->start(); });
      pthread_setname_np(threads.back().native_handle(),
                         ("Receiver " + std::to_string(i)).c_str());
      // GL: Set the affinity of the thread
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET(num_thread+i, &cpuset);
      int rc = pthread_setaffinity_np(threads.back().native_handle(), sizeof(cpu_set_t), &cpuset);
      if (rc != 0)
        fprintf(stderr, "Error calling pthread_setaffinity_np: %d.\n", rc);
      else
        fprintf(stderr, "Receiver thread %d was pinned to CPU core %d.\n", i, num_thread+i);
    }
    for (uint32_t i = 0; i < num_thread; i++) {
      threads[i].join();
    }
    DnsTesterAggregator aggregator(testers);
    aggregator.display();
    aggregator.write("dns64perf.csv");
  } catch (std::exception &e) {
    std::cerr << e.what() << std::endl;
  }
  return 0;
}
