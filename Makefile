CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g
LDFLAGS  = -libverbs -lrdmacm

all: rdma_server rdma_client bench_server bench_client

rdma_server: src/rdma_server.cpp src/common.hpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

rdma_client: src/rdma_client.cpp src/common.hpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

bench_server: src/bench_server.cpp src/common.hpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

bench_client: src/bench_client.cpp src/common.hpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f rdma_server rdma_client bench_server bench_client

.PHONY: all clean
