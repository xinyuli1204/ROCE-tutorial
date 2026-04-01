CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g
LDFLAGS  = -libverbs -lrdmacm

# CUDA paths (adjust if your CUDA is not in /usr/local/cuda)
CUDA_HOME   ?= /usr/local/cuda
CUDA_FLAGS   = -I$(CUDA_HOME)/include
CUDA_LIBS    = -L$(CUDA_HOME)/lib64 -lcudart

all: rdma_server rdma_client bench_server bench_client bench_server_gpu

rdma_server: src/rdma_server.cpp src/common.hpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

rdma_client: src/rdma_client.cpp src/common.hpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# CPU-only benchmark server (no CUDA dependency)
bench_server: src/bench_server.cpp src/common.hpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) -lpthread

# GPUDirect benchmark server (requires CUDA on server)
bench_server_gpu: src/bench_server.cpp src/common.hpp
	$(CXX) $(CXXFLAGS) $(CUDA_FLAGS) -DHAVE_CUDA -o $@ $< $(LDFLAGS) -lpthread $(CUDA_LIBS)

bench_client: src/bench_client.cpp src/common.hpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f rdma_server rdma_client bench_server bench_server_gpu bench_client

.PHONY: all clean
