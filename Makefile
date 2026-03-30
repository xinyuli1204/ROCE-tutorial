CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g
LDFLAGS  = -libverbs -lrdmacm

all: rdma_server rdma_client

rdma_server: rdma_server.cpp common.hpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

rdma_client: rdma_client.cpp common.hpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f rdma_server rdma_client

.PHONY: all clean
