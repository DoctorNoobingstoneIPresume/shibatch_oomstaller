// Written by Naoki Shibata  https://shibatch.github.io

#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <chrono>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <cmath>
#include <ctime>

#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/limits.h>

const long pageSize = sysconf(_SC_PAGESIZE);
uid_t uid = getuid();
const pid_t pid = getpid();

double period = 1.0;
int maxParallel = 0, maxParallelThrash = 1;

bool showStat = false;
std::unordered_map<int, long> statInfo;

std::uint64_t readMemInfo(const std::string_view &s) {
  std::FILE *fp = std::fopen("/proc/meminfo", "r");
  if (!fp) throw(std::runtime_error("readMeminfo() : could not open /proc/meminfo"));
  std::vector<char> line(1024);

  while(!std::feof(fp)) {
    if (std::fgets(line.data(), line.size(), fp) == NULL) break;
    if (std::strncmp(line.data(), s.data(), s.size()) == 0) {
      unsigned long long ull;
      if (std::sscanf(line.data() + s.size(), " %llu", &ull) != 1)
	throw(std::runtime_error("readMeminfo() : /proc/meminfo format error"));
      std::fclose(fp);
      return ull;
    }
  }
  throw(std::runtime_error(std::string("readMemInfo() : /proc/meminfo does not have entry for ") + s.data()));
}

struct ProcInfo {
  std::string comm, d_name;
  int pid, ppid, pgrp, session;
  long long unsigned starttime;
  long unsigned vsize, vmswap = 0;
  long int rss, num_threads;
  char state;

  ProcInfo() {}

  ProcInfo(std::FILE *fpstat, const char *dn) {
    d_name = dn;
    std::vector<char> line(1024);
    if (std::fgets(line.data(), line.size(), fpstat) == NULL)
      throw(std::runtime_error("Could not read /proc/<pid>/stat"));

    std::string ss = line.data();
    std::size_t ps = ss.find_first_of('('), pe = ss.find_last_of(')');
    if (ps == std::string::npos || pe == std::string::npos)
      throw(std::runtime_error("Could not read process name in /proc/<pid>/stat"));

    int n = std::sscanf(line.data(), "%d", &pid);
    if (n != 1) throw(std::runtime_error("Could not read pid in /proc/<pid>/stat"));

    //                                      3  4  5  6  7   8   9   10  11  12  13  14  15  16  17  18  19  20  21  22   23  24
    n = std::sscanf(line.data() + pe + 1, " %c %d %d %d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %ld %*d %llu %lu %ld",
	       &state, &ppid, &pgrp, &session, &num_threads, &starttime, &vsize, &rss);
    if (n != 8) throw(std::runtime_error("/proc/<pid>/stat format error"));

    comm = ss.substr(ps + 1, pe - ps - 1);

    //

    {
      std::FILE *fp = std::fopen((std::string("/proc/") + d_name + "/status").c_str(), "r");

      if (fp) {
	while(!std::feof(fp)) {
	  if (std::fgets(line.data(), line.size(), fp) == NULL) break;
	  if (std::strncmp(line.data(), "VmSwap:", 7) == 0) {
	    if (std::sscanf(line.data(), "%*s %lu", &vmswap) == 1) vmswap = vmswap * 1024 / pageSize;
	    break;
	  }
	}
	std::fclose(fp);
      }
    }

    //

    if (num_threads > 1) {
      DIR *taskdir = opendir((std::string("/proc/") + dn + "/task").c_str());
      if (!taskdir) return;

      struct dirent *entry;
      while((entry = readdir(taskdir))) {
	char *p;
	std::strtoul(entry->d_name, &p, 10);
	if (*p != '\0') continue;

	std::FILE *fp = std::fopen((std::string("/proc/") + dn + "/task/" + entry->d_name + "/stat").c_str(), "r");
	if (!fp) continue;

	if (std::fgets(line.data(), line.size(), fp) != NULL) {
	  pe = std::string(line.data()).find_last_of(')');
	  char c;
	  if (pe != std::string::npos && sscanf(line.data() + pe + 1, " %c", &c) == 1 &&
	      (c == 'R' || c == 'T' || c == 'D')) state = c;
	}

	std::fclose(fp);
      }

      closedir(taskdir);
    }
  }
};

std::unordered_map<int, ProcInfo> getProcesses() {
  std::unordered_map<int, ProcInfo> ret;

  DIR *procdir = opendir("/proc");
  if (!procdir) throw(std::runtime_error("opendir(\"/proc\") failed"));

  struct dirent *entry;
  struct stat statbuf;
  while((entry = readdir(procdir))) {
    char *p;
    std::strtoul(entry->d_name, &p, 10);
    if (*p != '\0') continue;

    std::FILE *fp = std::fopen((std::string("/proc/") + entry->d_name + "/stat").c_str(), "r");
    if (!fp) continue;

    if (fstat(fileno(fp), &statbuf) == 0 && statbuf.st_uid == uid) {
      try {
	ProcInfo pi(fp, entry->d_name);
	ret[pi.pid] = pi;
      } catch(std::exception &ex) {}
    }

    std::fclose(fp);
  }
  closedir(procdir);

  return ret;
}

bool isTarget(std::unordered_map<int, ProcInfo> &map, const ProcInfo *pc) {
  if (pc->pid == pid) return false;

  for(;;) {
    if (pc->pid == pid) return true;
    if (map.count(pc->ppid) == 0) return false;
    pc = &map[pc->ppid];
  }

  return false;
}

std::unordered_set<int> stoppedProcs;
volatile bool exiting = false;
std::mutex mtx;
std::condition_variable condvar;

void loop(std::shared_ptr<std::thread> childTh) {
  // Clear the thrashing detection if swap space is used but total
  // swap has not increased for 10 seconds
  const int THRASHCOUNT1 = 10.0 / period;

  // Clear the thrashing detection 3 seconds after total size of swap
  // space is reduced
  const int THRASHCOUNT2 = 3.0 / period;

  // The estimated amount of memory that each process could
  // potentially occupy is attenuated by this amount each time one
  // process finishes
  const double MEMMAXDECAY = pow(0.5, 1.0 / 10.0);

  //

  std::unique_lock<std::mutex> lock(mtx);
  std::unordered_set<int> lastActivePids;
  double memMax = 0;

  const long totalMem = readMemInfo("MemTotal:") * 1024 / pageSize;
  long lastSwapFree = readMemInfo("SwapFree:");
  int thrashTimer = 0;

  while(!exiting) {
    std::set<ProcInfo, bool(*)(const ProcInfo &lhs, const ProcInfo &rhs)>
    proc { [](const ProcInfo &lhs, const ProcInfo &rhs) {
      if (lhs.starttime > rhs.starttime) return true;
      if (lhs.starttime < rhs.starttime) return false;
      return lhs.pid > rhs.pid; } };

    long usedMem = 0, memMax2 = 0;
    int pidLargest = 0;
    std::unordered_set<int> activePids;
    bool swapUsed = false;

    {
      auto m = getProcesses();

      if (m.count(pid) == 0) throw(std::runtime_error("Could not retrieve process information of oomstaller"));

      for(auto e : m) {
	if ((e.second.state != 'R' && e.second.state != 'T' && e.second.state != 'D') ||
	    !isTarget(m, &e.second)) continue;
	if (e.second.rss + e.second.vmswap > (unsigned long)memMax2) memMax2 = e.second.rss + e.second.vmswap;
	usedMem += e.second.rss;
	if (e.second.vmswap > 0) swapUsed = true;
	proc.insert(e.second);
	activePids.insert(e.second.pid);
	if (pidLargest == 0 || (e.second.rss + e.second.vmswap > m.at(pidLargest).rss + m.at(pidLargest).vmswap))
	    pidLargest = e.second.pid;
      }
    }

    long freeMem = readMemInfo("MemAvailable:") * 1024 / pageSize, usableMem = freeMem + usedMem;
    if (usableMem > totalMem) usableMem = totalMem;

    // Detection of swap thrashing

    long swapFree = readMemInfo("SwapFree:");
    if (swapFree < lastSwapFree) {
      thrashTimer = THRASHCOUNT1;
    } else if (swapFree > lastSwapFree) {
      if (thrashTimer > THRASHCOUNT2) thrashTimer = THRASHCOUNT2;
    } else if (!swapUsed) {
      thrashTimer = 0;
    } else {
      if (thrashTimer > 0) thrashTimer--;
    }
    lastSwapFree = swapFree;

    // Calculate the number of parallel executions

    for(auto a : lastActivePids) if (activePids.count(a) == 0) memMax *= MEMMAXDECAY;
    lastActivePids = activePids;

    // memMax2 is the maximum occupied memory by the currently running processes
    // memMax is the estimated amount of memory that each process could potentially occupy
    if (memMax2 > memMax) memMax = memMax2;

    int maxParallelOP = INT_MAX;
    if (memMax != 0) maxParallelOP = usableMem / memMax + 1;

    // Determine the next state of each process

    std::unordered_set<int> removedPids;
    long n = proc.size();
    int nRunningProcs = 0;
    for(auto e : proc) {
      char nextState = '\0';
      if (e.pid == pidLargest) {
	nextState = 'R';
      } else {
	if (n > maxParallelOP || (maxParallel > 0 && n > maxParallel) ||
	    ((thrashTimer > 0 || freeMem == 0) && maxParallelThrash > 0 && n > maxParallelThrash)) {
	  nextState = 'T';
	  n--;
	} else {
	  nextState = 'R';
	}
      }

      if (nextState == 'R') {
	kill(e.pid, SIGCONT);
	stoppedProcs.erase(e.pid);
	nRunningProcs++;
      } else {
	stoppedProcs.insert(e.pid);
	kill(e.pid, SIGSTOP);
      }
    }

    // Handling of processes that have terminated or slept on its own after sending SIGSTOP

    for(auto i : stoppedProcs) if (activePids.count(i) == 0) removedPids.insert(i);

    for(auto i : removedPids) {
      kill(i, SIGCONT);
      stoppedProcs.erase(i);
    }

    // Update statistics info

    if (thrashTimer > 0 || freeMem == 0) statInfo[-1]++;
    statInfo[nRunningProcs]++;

    //

    condvar.wait_for(lock, std::chrono::milliseconds((long)(period * 1000)));
  }
}

int childExitCode = -1;

void execChild(const std::string_view &cmd) {
  childExitCode = WEXITSTATUS(system(cmd.data()));
  exiting = true;

  std::unique_lock<std::mutex> lock(mtx);
  condvar.notify_all();
}

void handlerThread(int n) {
  try {
    auto m = getProcesses();
    for(auto e : m) {
      if (e.second.ppid != pid) continue;
      kill(-e.second.pgrp, SIGCONT);
      kill(-e.second.pgrp, n);
    }
  } catch(std::exception &ex) {
    std::cerr << ex.what() << std::endl;
  }

  std::unique_lock<std::mutex> lock(mtx);
  for(auto e : stoppedProcs) kill(e, SIGCONT);
}

std::shared_ptr<std::thread> handlerTh;

void handler(int n) {
  signal(SIGINT , SIG_IGN);
  signal(SIGTERM, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGHUP , SIG_IGN);

  handlerTh = std::make_shared<std::thread>(handlerThread, n);
}

void showUsage(const std::string_view& argv0, const std::string_view& mes = std::string_view{}) {
  if (! mes.empty()) std::cerr << mes << std::endl << std::endl;
  std::cerr << std::endl;
  std::cerr << "NAME" << std::endl;
  std::cerr << "     oomstaller - suppress swap thrashing at build time" << std::endl;
  std::cerr << std::endl;
  std::cerr << "SYNOPSIS" << std::endl;
  std::cerr << "     " << argv0 << " [<options>] command [arg] ..." << std::endl;
  std::cerr << std::endl;
  std::cerr << "DESCRIPTION" << std::endl;
  std::cerr << "     This tool monitors the memory usage of each process when performing a" << std::endl;
  std::cerr << "     build, and suspends processes as necessary to prevent swap thrashing" << std::endl;
  std::cerr << "     from occurring." << std::endl;
  std::cerr << std::endl;
  std::cerr << "  --max-parallel <number of processes>            default:   0" << std::endl;
  std::cerr << std::endl;
  std::cerr << "     Suspends processes so that the number of running build processes" << std::endl;
  std::cerr << "     does not exceed the specified number. 0 means no limit. A process is" << std::endl;
  std::cerr << "     counted as one process even if it has multiple threads." << std::endl;
  std::cerr << std::endl;
  std::cerr << "  --max-parallel-thrash <number of processes>     default:   1" << std::endl;
  std::cerr << std::endl;
  std::cerr << "     Specifies the maximum number of processes to run when thrashing is" << std::endl;
  std::cerr << "     detected. 0 means no limit." << std::endl;
  std::cerr << std::endl;
  std::cerr << "  --period <seconds>                              default:   1.0" << std::endl;
  std::cerr << std::endl;
  std::cerr << "     Specifies the interval at which memory usage of each process is checked" << std::endl;
  std::cerr << "     and processes are controlled." << std::endl;
  std::cerr << std::endl;
  std::cerr << "  --show-stat" << std::endl;
  std::cerr << std::endl;
  std::cerr << "     Displays statistics when finished." << std::endl;
  std::cerr << std::endl;
  std::cerr << "TIPS" << std::endl;
  std::cerr << "     If you kill this tool with SIGKILL, a large number of build processes" << std::endl;
  std::cerr << "     will remain suspended with SIGSTOP. To prevent this from happening," << std::endl;
  std::cerr << "     use SIGTERM or SIGINT to kill this tool. You can send SIGCONT to all" << std::endl;
  std::cerr << "     processes run by you with the following command." << std::endl;
  std::cerr << std::endl;
  std::cerr << "     killall -v -s CONT -u $USER -r '.*'" << std::endl;
  std::cerr << std::endl;
  std::cerr << "AUTHOR" << std::endl;
  std::cerr << "     Written by Naoki Shibata." << std::endl;
  std::cerr << std::endl;
  std::cerr << "     See https://github.com/shibatch/oomstaller" << std::endl;
  std::cerr << std::endl;
  std::cerr << "oomstaller 0.4.0" << std::endl;
  std::cerr << std::endl;

  exit(-1);
}

int main(int argc, char **argv) {
  if (argc < 2) showUsage(argv[0]);

  int nextArg;
  for(nextArg = 1;nextArg < argc;nextArg++) {
    if (std::string(argv[nextArg]) == "--max-parallel") {
      if (nextArg+1 >= argc) showUsage(argv[0]);
      char *p;
      maxParallel = strtol(argv[nextArg+1], &p, 0);
      if (p == argv[nextArg+1] || *p || maxParallel < 0)
	showUsage(argv[0], "A non-negative integer is expected after --max-parallel.");
      nextArg++;
    } else if (std::string(argv[nextArg]) == "--max-parallel-thrash") {
      if (nextArg+1 >= argc) showUsage(argv[0]);
      char *p;
      maxParallelThrash = std::strtol(argv[nextArg+1], &p, 0);
      if (p == argv[nextArg+1] || *p || maxParallelThrash < 0)
	showUsage(argv[0], "A non-negative integer is expected after --max-parallel-thrash.");
      nextArg++;
    } else if (std::string(argv[nextArg]) == "--period") {
      if (nextArg+1 >= argc) showUsage(argv[0]);
      char *p;
      period = std::strtod(argv[nextArg+1], &p);
      if (p == argv[nextArg+1] || *p || period <= 0)
	showUsage(argv[0], "A positive value is expected after --period.");
      nextArg++;
    } else if (std::string(argv[nextArg]) == "--uid") {
      if (nextArg+1 >= argc) showUsage(argv[0]);
      char *p;
      long l = std::strtol(argv[nextArg+1], &p, 0);
      uid = l;
      if (p == argv[nextArg+1] || *p || l < 0)
	showUsage(argv[0], "A non-negative integer is expected after --uid.");
      nextArg++;
    } else if (std::string(argv[nextArg]) == "--show-stat") {
      showStat = true;
    } else if (std::string(argv[nextArg]).substr(0, 2) == "--") {
      showUsage(argv[0], std::string("Unrecognized option : ") + argv[nextArg]);
    } else {
      break;
    }
  }

  if (nextArg >= argc) showUsage(argv[0]);

  if (getProcesses().count(pid) == 0) {
    std::cerr << argv[0] << " : Could not retrieve process information of oomstaller" << std::endl;
    exit(-1);
  }

  auto startTime = std::chrono::system_clock::now();

  signal(SIGINT , handler);
  signal(SIGTERM, handler);
  signal(SIGQUIT, handler);
  signal(SIGHUP , handler);

  std::string cmd = argv[nextArg];
  for(int i=nextArg+1;i<argc;i++) {
    std::string in = argv[i], out = "'";
    for(int i=0;i<(int)in.length();i++) {
      if (in[i] == '\'') {
	out += "'\\''";
      } else {
	out += in[i];
      }
    }
    cmd = cmd + " " + out + "'";
  }

  auto childTh = std::make_shared<std::thread>(execChild, cmd);

  try {
    loop(childTh);
  } catch(std::exception &ex) {
    std::cerr << argv[0] << " : " << ex.what() << std::endl;
    kill(0, SIGTERM);
  }

  childTh->join();
  if (handlerTh) handlerTh->join();

  std::unique_lock<std::mutex> lock(mtx);
  for(auto e : stoppedProcs) kill(e, SIGCONT);

  auto endTime = std::chrono::system_clock::now();

  if (showStat) {
    if (statInfo[0] > 0) statInfo[0]--;
    if (statInfo[0] == 0) statInfo.erase(0);

    time_t tStartTime = std::chrono::system_clock::to_time_t(startTime);
    time_t tEndTime = std::chrono::system_clock::to_time_t(endTime);

    std::cout << std::endl;
    std::cout << "Start   : " << ctime(&tStartTime);
    std::cout << "End     : " << ctime(&tEndTime);

    std::chrono::duration<double> elapsed = endTime - startTime;
    std::cout << "Elapsed : " << elapsed.count() << " seconds" << std::endl;

    std::set<int> statKeys;
    for(auto a : statInfo) statKeys.insert(a.first);
    long pt = 0;
    for(auto a : statKeys) {
      if (a == -1) continue;
      pt += statInfo[a];
      std::cout << a << " processes running : " << statInfo[a] << " periods" << std::endl;
    }
    std::cout << "Thrashing detected : " << statInfo[-1] << " periods" << std::endl;
    std::cout << "Total : " << pt << " periods" << std::endl;
  }

  return childExitCode;
}
