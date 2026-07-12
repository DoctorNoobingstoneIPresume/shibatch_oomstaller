oomstaller : oomstaller.cpp
	$(CXX) -pthread -Os -Wall $(CXXFLAGS) oomstaller.cpp -o oomstaller

clean :
	rm -f oomstaller *~ *.s *.o *.out
