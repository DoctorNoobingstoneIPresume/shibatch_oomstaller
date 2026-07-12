oomstaller : oomstaller.cpp
	$(CXX) -Os -Wall $(CXXFLAGS) oomstaller.cpp -o oomstaller

clean :
	rm -f oomstaller *~ *.s *.o *.out
