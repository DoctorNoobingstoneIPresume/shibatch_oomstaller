oomstaller : oomstaller.cpp
	$(CXX) -pthread -std='gnu++17' -Os -Wall $(CXXFLAGS) -o 'oomstaller' \
		'readMemInfo.cpp' \
		'oomstaller.cpp'

clean :
	rm -f oomstaller *~ *.s *.o *.out
