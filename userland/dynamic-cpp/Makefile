CXX = g++-dynamic
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra -Werror
all: cpp-check
libcpp-service.so: service.cpp api.hpp
	$(CXX) $(CXXFLAGS) -fPIC -shared service.cpp -Wl,-soname,$@ -o $@
libcpp-plugin.so: plugin.cpp api.hpp libcpp-service.so
	$(CXX) $(CXXFLAGS) -fPIC -shared plugin.cpp -L. -lcpp-service -Wl,-rpath,'$$ORIGIN' -Wl,-soname,$@ -o $@
libcpp-unresolved.so: unresolved.cpp
	$(CXX) $(CXXFLAGS) -fPIC -shared $< -Wl,-soname,$@ -o $@
cpp-check: check.cpp api.hpp libcpp-service.so libcpp-plugin.so libcpp-unresolved.so
	$(CXX) $(CXXFLAGS) check.cpp -L. -lcpp-service -Wl,-rpath,'$$ORIGIN' -ldl -o $@
check: cpp-check
	./cpp-check
