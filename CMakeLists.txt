cmake_minimum_required (VERSION 3.0)
project (oomstaller)

add_executable ("oomstaller")
target_sources (
	"oomstaller"
	
	PRIVATE
		"oomstaller.cpp"
		"ProcInfo.cpp"
		"readMemInfo.cpp"
)
