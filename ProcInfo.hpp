#pragma once

#include <string>

#include <cstdio>

struct ProcInfo {
  std::string comm, d_name;
  int pid, ppid, pgrp, session;
  long long unsigned starttime;
  long unsigned vsize, vmswap = 0;
  long int rss, num_threads;
  char state;

  ProcInfo();

  ProcInfo(std::FILE *fpstat, const char *dn, long pageSize);
};
