#include <cstdio>
#include <cstring>
#include <isa-l.h>
#include <infiniband/verbs.h>
#include <cassert>

const int K = 4;        // # data chunks
const int M = 2;        // # parity chunks (must <= K)
const int S = K + M;    // # chunks in a stripe

const size_t SIZE = 64; // size in bytes of a chunk

int main(int argc, char **argv)
{
    int rc;

    // Assume only one Infiniband Adapter, so num_devices-1 == 0
    int num_devices = -1;
    ibv_device **dev_list = ibv_get_device_list(&num_devices);
    ibv_device *device = dev_list[num_devices - 1];
    ibv_free_device_list(dev_list);

    ibv_context *ctx = ibv_open_device(device);
    ibv_pd *pd = ibv_alloc_pd(ctx);

    // Generate encode/decode matrices with ISA-L
    // Assume the first M data blocks are "lost"
    uint8_t encode_matrix[S * K];
    gf_gen_cauchy1_matrix(encode_matrix, S, K);

    uint8_t encode_part_matrix[K * K], invert_matrix[K * K], decode_matrix[K * M];
    memcpy(encode_part_matrix, encode_matrix + (K * M), K * K);
    gf_invert_matrix(encode_part_matrix, invert_matrix, K);
    memcpy(decode_matrix, invert_matrix, K * M);

    // NIC EC offload requires the matrices be transposed
    uint8_t nic_encode_matrix[M * K], nic_decode_matrix[M * K];
    for (int i = 0; i < M; ++i) {
        for (int j = 0; j < K; ++j) {
            nic_encode_matrix[j * M + i] = encode_matrix[(i + K) * K + j];
            nic_decode_matrix[j * M + i] = invert_matrix[i * K + j];
        }
    }

    // Allocate buffer to store data & parity chunks
    uint8_t chunk_buf[S * SIZE * 2];

    // The buffer must be registered as locally writable by the NIC
    ibv_mr *mr = ibv_reg_mr(pd, chunk_buf, sizeof(chunk_buf), IBV_ACCESS_LOCAL_WRITE);

    uint8_t *chunks[S * 2];
    uint8_t **chunks2 = chunks + S;
    for (int i = 0; i < S * 2; ++i) {
        chunks[i] = chunk_buf + (i * SIZE);
    }

    // Allocate ibv_exp_ec_calc
    // Meanings of some of the parameters are still unclear...
    ibv_exp_ec_calc_init_attr ec_calc_init_attr;
    memset(&ec_calc_init_attr, 0, sizeof(ec_calc_init_attr));
    ec_calc_init_attr.comp_mask = IBV_EXP_EC_CALC_ATTR_MAX_INFLIGHT |           // maximum inflight calculations    (have an unknown upper bound?)
                                  IBV_EXP_EC_CALC_ATTR_K |                      // # data blocks
                                  IBV_EXP_EC_CALC_ATTR_M |                      // # parity blocks
                                  IBV_EXP_EC_CALC_ATTR_W |                      // Galois field bits (2^w)          (uint8_t -> must be 8)
                                  IBV_EXP_EC_CALC_ATTR_MAX_DATA_SGE |           // must equal to # data blocks
                                  IBV_EXP_EC_CALC_ATTR_MAX_CODE_SGE |           // must equal to # parity block
                                  IBV_EXP_EC_CALC_ATTR_ENCODE_MAT |             // encode matrix
                                  IBV_EXP_EC_CALC_ATTR_AFFINITY |               // affinity hint for asynchronous calcs completion steering (?)
                                  IBV_EXP_EC_CALC_ATTR_POLLING;                 // polling mode (if set no completions will be generated by events)
    ec_calc_init_attr.max_inflight_calcs = 1;
    ec_calc_init_attr.k = K;
    ec_calc_init_attr.m = M;
    ec_calc_init_attr.w = 8;
    ec_calc_init_attr.max_data_sge = K;
    ec_calc_init_attr.max_code_sge = M;
    ec_calc_init_attr.affinity_hint = 0;
    ec_calc_init_attr.encode_matrix = nic_encode_matrix;

    ibv_exp_ec_calc *ec_calc = ibv_exp_alloc_ec_calc(pd, &ec_calc_init_attr);
    if (!ec_calc) {
        fprintf(stderr, "failed to create ec_calc\n");
        exit(-1);
    }

    // Verify the correctness of ISA-L
    {
        // Initialize data chunks
        memset(chunk_buf, 0, sizeof(chunk_buf));
        for (int i = 0; i < K; ++i) {
            memset(chunks[i], i + 1, SIZE);
        }

        // Encode
        uint8_t tbls[K * M * 32];
        ec_init_tables(K, M, encode_matrix + (K * K), tbls);
        ec_encode_data(SIZE, K, M, tbls, chunks, chunks + K);
        printf("ISA-L encode: ");
        for (int i = 0; i < S; ++i) {
            if (i == K) {
                printf("-> ");
            }
            printf("%02x ", chunks[i][0]);
        }
        printf("\n");

        // Decode
        memset(chunk_buf, 0, K * M);
        ec_init_tables(K, M, decode_matrix, tbls);
        ec_encode_data(SIZE, K, M, tbls, chunks + M, chunks);
        for (int i = 0; i < M; ++i) {
            if (chunks[i][0] != i + 1) {
                fprintf(stderr, "decode matrix incorrect: chunks[%d] %02x != %02x\n", i, chunks[i][0], i + 1);
                exit(-1);
            }
        }
        printf("ISA-L decode: ok\n");
        printf("\n");
    }

    // Verify the correctness of NIC EC encode
    {
        // Initialize data chunks, should be the same to that of ISA-L
        memset(chunk_buf, 0, sizeof(chunk_buf));
        for (int i = 0; i < K; ++i) {
            memset(chunks[i], i + 1, SIZE);
        }

        ibv_sge sge[S];
        for (int i = 0; i < S; ++i) {
            sge[i].addr = reinterpret_cast<uintptr_t>(chunks[i]);
            sge[i].length = SIZE;
            sge[i].lkey = mr->lkey;
        }

        ibv_exp_ec_mem ec_mem;
        ec_mem.data_blocks = sge;
        ec_mem.code_blocks = sge + K;
        ec_mem.num_data_sge = K;
        ec_mem.num_code_sge = M;
        ec_mem.block_size = SIZE;

        if (ibv_exp_ec_encode_sync(ec_calc, &ec_mem)) {
            fprintf(stderr, "failed to encode\n");
            exit(-1);
        }
        
        for (int i = K; i < S; ++i) {
            for (size_t j = 1; j < SIZE; ++j) {
                if (chunks[i][j] != chunks[i][0]) {
                    fprintf(stderr, "encoding seems inconsistent: chunks[%d][%lu] %02x != chunks[%d][0] %02x\n", i, j, chunks[i][j], i, chunks[i][0]);
                    exit(-1);
                }
            }
        }
        printf("NIC encode:   ");
        for (int i = 0; i < S; ++i) {
            if (i == K) {
                printf("-> ");
            }
            printf("%02x ", chunks[i][0]);
        }
        printf("\n");
    }

    // Verify the correctness of NIC EC decode
    {
        for (int i = M; i < S; ++i) {
            memcpy(chunks2[i], chunks[i], SIZE);
        }

        ibv_sge sge[S];
        for (int i = 0; i < S; ++i) {
            sge[i].addr = reinterpret_cast<uintptr_t>(chunks2[i]);
            sge[i].length = SIZE;
            sge[i].lkey = mr->lkey;
        }

        ibv_exp_ec_mem ec_mem;
        ec_mem.data_blocks = sge;
        ec_mem.code_blocks = sge + K;
        ec_mem.num_data_sge = K;
        ec_mem.num_code_sge = M;
        ec_mem.block_size = SIZE;

        uint8_t erasures[S];
        memset(erasures, 0, sizeof(erasures));
        for (int i = 0; i < M; ++i) {
            erasures[i] = 1;
            memset(chunks2[i], 0, SIZE);
        }

        if (ibv_exp_ec_decode_sync(ec_calc, &ec_mem, erasures, nic_decode_matrix)) {
            fprintf(stderr, "failed to decode\n");
            exit(-1);
        }

        for (int i = 0; i < M; ++i) {
            for (size_t j = 0; j < SIZE; ++j) {
                if (chunks2[i][j] != chunks[i][j]) {
                    fprintf(stderr, 
                        "decoding seems inconsistent: chunks2[%d][%lu] %02x != chunks[%d][%lu] %02x\n",
                        i, j, chunks2[i][j], i, j, chunks[i][0]
                    );
                    exit(-1);
                }
            }
        }
        printf("NIC decode:   ok\n");
    }

    ibv_exp_dealloc_ec_calc(ec_calc);
    ibv_dereg_mr(mr);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    
    return 0;
}
