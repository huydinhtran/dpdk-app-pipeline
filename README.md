## build DPDK 20.11.0 (tested with RDMA v22)
cd dpdk-pipeline-macswap

meson setup build

cd build

ninja

sudo ninja install

cd .. && export RTE_SDK=$(pwd)
