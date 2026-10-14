CC = gcc
CXX = g++
CFLAGS = -O2 -Wall -Wextra
CXXFLAGS = $(CFLAGS) -std=c++17

.PHONY: all check clean
all: hello-c hello-cxx abi-check
hello-c: hello.o
	$(CC) $< -o $@
hello-cxx: hello-cxx.o
	$(CXX) $< -o $@
hello-cxx.o: hello.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@
abi-check: abi-check.c include/memory_usage.h
	$(CC) $(CFLAGS) $< -o $@
check: all
	./hello-c
	./hello-cxx
	./abi-check
clean:
	rm -f hello.o hello-cxx.o hello-c hello-cxx abi-check
