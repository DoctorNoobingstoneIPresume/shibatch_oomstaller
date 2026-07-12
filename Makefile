oomstaller : oomstaller.cpp
	$(CXX) -pthread -std='gnu++17' -Os -Wall $(CXXFLAGS) oomstaller.cpp -o oomstaller

clean :
	rm -f oomstaller *~ *.s *.o *.out
