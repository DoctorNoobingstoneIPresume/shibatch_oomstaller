#include "ProcInfo.hpp"

#include <vector>
#include <string>
#include <stdexcept>

#include <cstring>
#include <cstdlib>

#include <sys/types.h>
#include <dirent.h>

ProcInfo::ProcInfo() {}

ProcInfo::ProcInfo(std::FILE *fpstat, const char *dn, long pageSize) {
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
        if (pe != std::string::npos && sscanf(line.data() + pe + 1, " %c", &c) == 1 && (c == 'R' || c == 'T' || c == 'D'))
          state = c;
      }

      std::fclose(fp);
    }

    closedir(taskdir);
  }
}
