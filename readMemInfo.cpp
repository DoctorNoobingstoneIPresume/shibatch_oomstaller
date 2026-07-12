#include "readMemInfo.hpp"

#include <vector>
#include <string>
#include <string_view>
#include <stdexcept>

#include <cstdio>
#include <cstring>

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
