# Code Example for CX-5+ Erasure Coding Offload

## Dependency

* [Intel ISA-L](https://github.com/intel/isa-l)
* [MLNX_OFED](https://network.nvidia.com/products/infiniband-drivers/linux/mlnx_ofed/) **IMPORTANT: version must be v4.x!** (recommended v4.7-3.2.9.0)

## Build & run

```shell
make
./test
```

Expected output:
```shell
ISA-L encode: 01 02 03 04 -> 48 0f 
ISA-L decode: ok

NIC encode:   01 02 03 04 -> 48 0f 
NIC decode:   ok
```
