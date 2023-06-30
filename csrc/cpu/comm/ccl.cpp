// Copyright (c) Microsoft Corporation.
// SPDX-License-Identifier: Apache-2.0

// DeepSpeed Team

#include <torch/extension.h>

#include <fcntl.h>
#include <immintrin.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <oneapi/ccl.hpp>

struct SharedData {
    const char* name;
    int descriptor;
    void* bytes;
    size_t nbytes;
};

int world_rank = -1;
int world_size = -1;

void shared_open(SharedData* data, const char* name, size_t nbytes)
{
    int d = shm_open(name, O_RDWR, S_IRUSR | S_IWUSR);
    if (d != -1) {
        void* bytes = mmap(NULL, nbytes, PROT_READ | PROT_WRITE, MAP_SHARED, d, 0);
        data->name = name;
        data->descriptor = d;
        data->bytes = bytes;
        data->nbytes = nbytes;
    } else {
        printf("(%d)shared_open %s failed\n", world_rank, name);
        data->descriptor = -1;
    }
}

void shared_create(SharedData* data, const char* name, void* bytes, size_t nbytes)
{
    int d = shm_open(name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (d != -1) {
        if (nbytes = write(d, bytes, nbytes)) { shared_open(data, name, nbytes); }
    } else {
        printf("(%d)shared_create %s failed\n", world_rank, name);
    }
}

void shared_close(SharedData* data)
{
    if (data->descriptor != -1) {
        munmap(data->bytes, data->nbytes);
        shm_unlink(data->name);
    }
}

std::set<int> _comm_ids;
std::set<int> _colors;
ccl::vector_class<ccl::communicator> _ccl_comms;

ccl::communicator& _get_comm_from_group() { return _ccl_comms[0]; }
ccl::communicator& _get_comm_from_group(py::object group) { return _ccl_comms[0]; }

#define CCLCHECK(cmd) \
    do {              \
        cmd;          \
    } while (0)

#define KVS_CREATE_SUCCESS 0
#define KVS_CREATE_FAILURE -1

bool is_initialized = 0;

ccl::shared_ptr_class<ccl::kvs> kvs;

SharedData allreduce_buffer;
char buffer_name[100] = "allreduce_buffer";
struct allreduce_workspace {
    int state;
    char buffer[32768];
};
struct allreduce_workspace* buffer;

void initialize(int size, int rank, torch::Tensor& kvs_data)
{
    if (is_initialized) return;
    world_size = size;
    world_rank = rank;
    is_initialized = 1;

    ccl::kvs::address_type main_addr;

    if (rank != 0) {
        memcpy(main_addr.data(), kvs_data.data_ptr(), main_addr.size());
        kvs = ccl::create_kvs(main_addr);
    }

    _ccl_comms.emplace_back(ccl::create_communicator(size, rank, kvs));

    if (rank == 0) {
        buffer = (struct allreduce_workspace*)malloc(size * sizeof(struct allreduce_workspace));
        shared_create(
            &allreduce_buffer, buffer_name, buffer, size * sizeof(struct allreduce_workspace));
        buffer = (struct allreduce_workspace*)allreduce_buffer.bytes;
        for (int i = 0; i < size; i++) { buffer[i].state = 0; }
    }
    CCLCHECK(ccl::barrier(_get_comm_from_group()).wait());
    if (rank != 0) {
        shared_open(&allreduce_buffer, buffer_name, size * sizeof(struct allreduce_workspace));
    }
    buffer = (struct allreduce_workspace*)allreduce_buffer.bytes;
}

/*
    rank == 0: create main kvs and return its address
    rank == else: return an empty address
*/
std::vector<uint8_t> get_kvs_addr(int rank)
{
    if (rank == 0) {
        kvs = ccl::create_main_kvs();
        ccl::kvs::address_type main_addr = kvs->get_address();
        auto ccl_kvs_addr = std::vector<uint8_t>(main_addr.begin(), main_addr.end());
        return ccl_kvs_addr;
    } else {
        ccl::kvs::address_type main_addr;
        auto ccl_kvs_addr = std::vector<uint8_t>(main_addr.begin(), main_addr.end());
        return ccl_kvs_addr;
    }
}

int get_rank(int group = 0) { return world_rank; }

int get_world_size(int group = 0) { return world_size; }

// Find the next ordered, unique value to a set. E.g. <0,1,2,7> --> 3
int next_unique_val(std::set<int> s)
{
    std::set<int>::iterator itr;
    // Base case. Add 0 to start of set.
    if (s.empty() || *s.begin() != 0) {
        return 0;
        // second base case where s = {0} (the case of s = {n != 0} is caught above)
    } else if (s.size() == 1) {
        return 1;
    } else {
        int prev_val = *s.begin();
        for (itr = std::next(s.begin()); itr != s.end(); itr++) {
            if (*itr != prev_val + 1) { return prev_val + 1; }
            prev_val = *itr;
        }
        return *(s.end()) + 1;
    }
}

py::object new_group(std::vector<int> ranks)
{
    int comm_id = next_unique_val(_comm_ids);
    int color = next_unique_val(_colors);
    std::cout << "RANK: " << get_rank() << " COMM_ID: " << comm_id << " COLOR: " << color
              << std::endl;
}

ccl::datatype get_ccl_datatype(c10::ScalarType type)
{
    ccl::datatype ccl_type;
    switch (type) {
        case c10::ScalarType::Int: ccl_type = ccl::datatype::int32; break;
        case c10::ScalarType::Float: ccl_type = ccl::datatype::float32; break;
        case c10::ScalarType::Double: ccl_type = ccl::datatype::float64; break;
        case c10::ScalarType::BFloat16: ccl_type = ccl::datatype::bfloat16; break;
        case c10::ScalarType::Half: ccl_type = ccl::datatype::float16; break;
        default: ccl_type = ccl::datatype::int8;
    }
    return ccl_type;
}

ccl::reduction get_ccl_reduce_op(py::object op, at::Tensor& input)
{
    py::object ReduceOp = py::module_::import("deepspeed.comm").attr("ReduceOp");
    if (!py::isinstance(op, ReduceOp)) {
        throw std::runtime_error("Error: Op must be of type ReduceOp");
    }

    int op_val = py::int_(op.attr("value"));
    ccl::reduction ccl_op;

    if (input.scalar_type() == at::kBool) {
        if (op_val == (int)py::int_(ReduceOp.attr("SUM").attr("value"))) {
            // For bool tensors, map sum to max, which both represent a bitwise or.
            // This is to prevent overflow issues with sum, since we use uint8 to
            // represent a bool (see cclDataType mapping).
            ccl_op = ccl::reduction::max;
        } else if (op_val == (int)py::int_(ReduceOp.attr("AVG").attr("value"))) {
            throw std::runtime_error("Error: For bool tensors, op must be of type ReduceOp");
        }
    }

    if (op_val == (int)py::int_(ReduceOp.attr("SUM").attr("value"))) {
        ccl_op = ccl::reduction::sum;
    } else if (op_val == (int)py::int_(ReduceOp.attr("MIN").attr("value"))) {
        ccl_op = ccl::reduction::min;
    } else if (op_val == (int)py::int_(ReduceOp.attr("MAX").attr("value"))) {
        ccl_op = ccl::reduction::max;
    } else if (op_val == (int)py::int_(ReduceOp.attr("PRODUCT").attr("value"))) {
        ccl_op = ccl::reduction::prod;
    } else {
        throw std::runtime_error("Error: Unrecognized ReduceOp type");
    }
    return ccl_op;
}

void broadcast(torch::Tensor& data, int src, py::object group, bool async_op)
{
    CCLCHECK(ccl::broadcast(data.data_ptr(),
                            data.numel(),
                            get_ccl_datatype(data.scalar_type()),
                            src,
                            _get_comm_from_group(group))
                 .wait());
}

float total = 0.0f;
float total_sq = 0.0f;
float min = 1000.0f;
float max = 0.0f;
int count = 0;
int min_count = 0;
int max_count = 0;
// TODO: implement torch's async_op behavior, document it.
void all_reduce(torch::Tensor& data, py::object op, py::object group, bool async_op)
{
    auto start = std::chrono::high_resolution_clock::now();
    CCLCHECK(ccl::allreduce(data.data_ptr(),
                            data.data_ptr(),
                            data.numel(),
                            get_ccl_datatype(data.scalar_type()),
                            get_ccl_reduce_op(op, data),
                            _get_comm_from_group(group))
                 .wait());
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    count++;
    auto t = duration.count();
    total += t;
    total_sq += t * t;
    auto segma = sqrt(total_sq / count - total * total / count / count);
    if (count == 17920 && world_rank == 0) {
        printf("average duration: %f, std: %f\n", total / count, segma);
    }
}

void wait_buffer_state_until(int index, int state)
{
    volatile int* state_ptr = &(buffer[index].state);

    while (*state_ptr != state)
        ;
}

__m512 cvt_bf16_to_fp32(const __m256i src) __attribute__((target("avx512bw")));
inline __m512 cvt_bf16_to_fp32(const __m256i src)
{
    auto y = _mm512_cvtepu16_epi32(src);
    return _mm512_castsi512_ps(_mm512_bslli_epi128(y, 2));
}

inline __m256i cvt_fp32_to_bf16(const __m512 src) __attribute__((target("avx512bw")));
inline __m256i cvt_fp32_to_bf16(const __m512 src)
{
// #if (defined CPU_CAPABILITY_AVX512_BF16)
#if 0
  return reinterpret_cast<__m256i>(_mm512_cvtneps_pbh(src));
#else
    __m512i value = _mm512_castps_si512(src);
    __m512i nan = _mm512_set1_epi32(0xffff);
    auto mask_value = _mm512_cmp_ps_mask(src, src, _CMP_ORD_Q);
    __m512i ones = _mm512_set1_epi32(0x1);
    __m512i vec_bias = _mm512_set1_epi32(0x7fff);
    // uint32_t lsb = (input >> 16) & 1;
    auto t_value = _mm512_and_si512(_mm512_srli_epi32(value, 16), ones);
    // uint32_t rounding_bias = 0x7fff + lsb;
    t_value = _mm512_add_epi32(t_value, vec_bias);
    // input += rounding_bias;
    t_value = _mm512_add_epi32(t_value, value);
    // input = input >> 16;
    t_value = _mm512_srli_epi32(t_value, 16);
    // Check NaN before converting back to bf16
    t_value = _mm512_mask_blend_epi32(mask_value, nan, t_value);
    return _mm512_cvtusepi32_epi16(t_value);
#endif
}

void reduce_bf16_buffers(void* in_out, void* in, int num_elements)
    __attribute__((target("avx512bw")));

void reduce_3_bf16_buffers(void* in_out, void* in1, void* in2, int num_elements)
    __attribute__((target("avx512bw")));
void reduce_4_bf16_buffers(void* in_out, void* in1, void* in2, void* in3, int num_elements)
    __attribute__((target("avx512bw")));
void reduce_5_bf16_buffers(void* in_out,
                           void* in1,
                           void* in2,
                           void* in3,
                           void* in4,
                           int num_elements) __attribute__((target("avx512bw")));
void reduce_6_bf16_buffers(void* in_out,
                           void* in1,
                           void* in2,
                           void* in3,
                           void* in4,
                           void* in5,
                           int num_elements) __attribute__((target("avx512bw")));
void reduce_7_bf16_buffers(void* in_out,
                           void* in1,
                           void* in2,
                           void* in3,
                           void* in4,
                           void* in5,
                           void* in6,
                           int num_elements) __attribute__((target("avx512bw")));
void reduce_8_bf16_buffers(void* in_out,
                           void* in1,
                           void* in2,
                           void* in3,
                           void* in4,
                           void* in5,
                           void* in6,
                           void* in7,
                           int num_elements) __attribute__((target("avx512bw")));

void reduce_all_bf16_buffers(struct allreduce_workspace* buffer, int num_elements, int num_buffers)
{
    if (num_buffers == 8) {
        reduce_8_bf16_buffers(buffer[0].buffer,
                              buffer[1].buffer,
                              buffer[2].buffer,
                              buffer[3].buffer,
                              buffer[4].buffer,
                              buffer[5].buffer,
                              buffer[6].buffer,
                              buffer[7].buffer,
                              num_elements);
    } else if (num_buffers == 7) {
        reduce_7_bf16_buffers(buffer[0].buffer,
                              buffer[1].buffer,
                              buffer[2].buffer,
                              buffer[3].buffer,
                              buffer[4].buffer,
                              buffer[5].buffer,
                              buffer[6].buffer,
                              num_elements);
    } else if (num_buffers == 6) {
        reduce_6_bf16_buffers(buffer[0].buffer,
                              buffer[1].buffer,
                              buffer[2].buffer,
                              buffer[3].buffer,
                              buffer[4].buffer,
                              buffer[5].buffer,
                              num_elements);
    } else if (num_buffers == 5) {
        reduce_5_bf16_buffers(buffer[0].buffer,
                              buffer[1].buffer,
                              buffer[2].buffer,
                              buffer[3].buffer,
                              buffer[4].buffer,
                              num_elements);
    } else if (num_buffers == 4) {
        reduce_4_bf16_buffers(
            buffer[0].buffer, buffer[1].buffer, buffer[2].buffer, buffer[3].buffer, num_elements);
    } else if (num_buffers == 3) {
        reduce_3_bf16_buffers(buffer[0].buffer, buffer[1].buffer, buffer[2].buffer, num_elements);
    } else {
        for (int i = 1; i < num_buffers; i++) {
            reduce_bf16_buffers(buffer[0].buffer, buffer[i].buffer, num_elements);
        }
    }
}

void reduce_bf16_buffers(void* in_out, void* in, int num_elements)
{
    for (int i = 0; i < num_elements * 2; i += 32) {
        auto in1 = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in + i)));
        auto inout1 = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in_out + i)));
        inout1 = _mm512_add_ps(inout1, in1);
        _mm256_storeu_si256((__m256i*)(in_out + i), cvt_fp32_to_bf16(inout1));
    }
}

void reduce_3_bf16_buffers(void* in_out, void* in1, void* in2, int num_elements)
{
    for (int i = 0; i < num_elements * 2; i += 32) {
        auto inout_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in_out + i)));
        auto in1_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in1 + i)));
        inout_val = _mm512_add_ps(inout_val, in1_val);
        auto in2_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in2 + i)));
        inout_val = _mm512_add_ps(inout_val, in2_val);
        _mm256_storeu_si256((__m256i*)(in_out + i), cvt_fp32_to_bf16(inout_val));
    }
}

void reduce_4_bf16_buffers(void* in_out, void* in1, void* in2, void* in3, int num_elements)
{
    for (int i = 0; i < num_elements * 2; i += 32) {
        auto inout_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in_out + i)));
        auto in1_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in1 + i)));
        inout_val = _mm512_add_ps(inout_val, in1_val);
        auto in2_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in2 + i)));
        inout_val = _mm512_add_ps(inout_val, in2_val);
        auto in3_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in3 + i)));
        inout_val = _mm512_add_ps(inout_val, in3_val);
        _mm256_storeu_si256((__m256i*)(in_out + i), cvt_fp32_to_bf16(inout_val));
    }
}

void reduce_5_bf16_buffers(void* in_out,
                           void* in1,
                           void* in2,
                           void* in3,
                           void* in4,
                           int num_elements)
{
    for (int i = 0; i < num_elements * 2; i += 32) {
        auto inout_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in_out + i)));
        auto in1_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in1 + i)));
        inout_val = _mm512_add_ps(inout_val, in1_val);
        auto in2_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in2 + i)));
        inout_val = _mm512_add_ps(inout_val, in2_val);
        auto in3_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in3 + i)));
        inout_val = _mm512_add_ps(inout_val, in3_val);
        auto in4_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in4 + i)));
        inout_val = _mm512_add_ps(inout_val, in4_val);
        _mm256_storeu_si256((__m256i*)(in_out + i), cvt_fp32_to_bf16(inout_val));
    }
}

void reduce_6_bf16_buffers(void* in_out,
                           void* in1,
                           void* in2,
                           void* in3,
                           void* in4,
                           void* in5,
                           int num_elements)
{
    for (int i = 0; i < num_elements * 2; i += 32) {
        auto inout_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in_out + i)));
        auto in1_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in1 + i)));
        inout_val = _mm512_add_ps(inout_val, in1_val);
        auto in2_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in2 + i)));
        inout_val = _mm512_add_ps(inout_val, in2_val);
        auto in3_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in3 + i)));
        inout_val = _mm512_add_ps(inout_val, in3_val);
        auto in4_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in4 + i)));
        inout_val = _mm512_add_ps(inout_val, in4_val);
        auto in5_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in5 + i)));
        inout_val = _mm512_add_ps(inout_val, in5_val);
        _mm256_storeu_si256((__m256i*)(in_out + i), cvt_fp32_to_bf16(inout_val));
    }
}

void reduce_7_bf16_buffers(void* in_out,
                           void* in1,
                           void* in2,
                           void* in3,
                           void* in4,
                           void* in5,
                           void* in6,
                           int num_elements)
{
    for (int i = 0; i < num_elements * 2; i += 32) {
        auto inout_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in_out + i)));
        auto in1_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in1 + i)));
        inout_val = _mm512_add_ps(inout_val, in1_val);
        auto in2_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in2 + i)));
        inout_val = _mm512_add_ps(inout_val, in2_val);
        auto in3_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in3 + i)));
        inout_val = _mm512_add_ps(inout_val, in3_val);
        auto in4_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in4 + i)));
        inout_val = _mm512_add_ps(inout_val, in4_val);
        auto in5_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in5 + i)));
        inout_val = _mm512_add_ps(inout_val, in5_val);
        auto in6_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in6 + i)));
        inout_val = _mm512_add_ps(inout_val, in6_val);
        _mm256_storeu_si256((__m256i*)(in_out + i), cvt_fp32_to_bf16(inout_val));
    }
}

void reduce_8_bf16_buffers(void* in_out,
                           void* in1,
                           void* in2,
                           void* in3,
                           void* in4,
                           void* in5,
                           void* in6,
                           void* in7,
                           int num_elements)
{
    for (int i = 0; i < num_elements * 2; i += 32) {
        auto inout_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in_out + i)));
        auto in1_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in1 + i)));
        inout_val = _mm512_add_ps(inout_val, in1_val);
        auto in2_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in2 + i)));
        inout_val = _mm512_add_ps(inout_val, in2_val);
        auto in3_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in3 + i)));
        inout_val = _mm512_add_ps(inout_val, in3_val);
        auto in4_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in4 + i)));
        inout_val = _mm512_add_ps(inout_val, in4_val);
        auto in5_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in5 + i)));
        inout_val = _mm512_add_ps(inout_val, in5_val);
        auto in6_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in6 + i)));
        inout_val = _mm512_add_ps(inout_val, in6_val);
        auto in7_val = cvt_bf16_to_fp32(_mm256_loadu_si256((__m256i*)(in7 + i)));
        inout_val = _mm512_add_ps(inout_val, in7_val);
        _mm256_storeu_si256((__m256i*)(in_out + i), cvt_fp32_to_bf16(inout_val));
    }
}

void all_reduce_low_latency(torch::Tensor& data, py::object op, py::object group, bool async_op)
{
    auto data_ptr = data.data_ptr();
    auto numel = data.numel();
    auto datatype = data.scalar_type();
    auto reduce_op = op;

    auto start = std::chrono::high_resolution_clock::now();
    memcpy(buffer[world_rank].buffer, data_ptr, numel * 2);
    buffer[world_rank].state = 1;

    if (world_rank == 0) {
        // compute allreduce result on rank 0
        for (int i = 1; i < world_size; i++) {
            // wait until the other rank copy the buffer
            wait_buffer_state_until(i, 1);
        }
        reduce_all_bf16_buffers(buffer, numel, world_size);
        // for (int i=1; i< world_size; i++) {
        //     reduce_bf16_buffers(buffer[0].buffer, buffer[i].buffer, numel);
        // }
        buffer[world_rank].state = 2;
        memcpy(data_ptr, buffer[0].buffer, numel * 2);
    }
    if (world_rank != 0) {
        wait_buffer_state_until(0, 2);
        memcpy(data_ptr, buffer[0].buffer, numel * 2);
        buffer[world_rank].state = 2;
    }
    if (world_rank == 0) {
        for (int i = 1; i < world_size; i++) { wait_buffer_state_until(i, 2); }
        buffer[world_rank].state = 0;
    }
    if (world_rank != 0) {
        wait_buffer_state_until(0, 0);
        buffer[world_rank].state = 0;
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    count++;
    auto t = duration.count();
    total += t;
    total_sq += t * t;
    auto segma = sqrt(total_sq / count - total * total / count / count);
    if (count == 17920 && world_rank == 0) {
        printf("average duration: %f, std: %f\n", total / count, segma);
    }
}

void all_reduce_caching(torch::Tensor& data,
                        py::object op,
                        std::string match_id,
                        py::object group,
                        bool async_op)
{
    ccl::allreduce_attr attr = ccl::default_allreduce_attr;
    auto match_str = ccl::v1::string(match_id);
    attr.template set<ccl::operation_attr_id::to_cache>(true);
    attr.template set<ccl::operation_attr_id::match_id>(match_str);
    // To control this, use operation attribute and set true value for to_cache field and unique
    // string (for example, tensor name) for match_id field. Note that:
    //   match_id should be the same for a specific communication operation across all ranks.
    //   If the same tensor is a part of different communication operations, match_id should have
    //   different values for each of these operations.
    CCLCHECK(ccl::allreduce(data.data_ptr(),
                            data.data_ptr(),
                            data.numel(),
                            get_ccl_datatype(data.scalar_type()),
                            get_ccl_reduce_op(op, data),
                            _get_comm_from_group(group),
                            attr)
                 .wait());
}

void barrier(py::object group, bool async_op)
{
    CCLCHECK(ccl::barrier(_get_comm_from_group(group)).wait());
}

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m)
{
    m.def("get_kvs_addr", &get_kvs_addr, "create and get main kvs addr");
    m.def("initialize", &initialize, "ccl initialize");
    m.def("get_rank", &get_rank, "get rank");
    m.def("get_world_size", &get_world_size, "get world size");
    m.def("broadcast", &broadcast, "ccl broadcast");
    m.def("all_reduce", &all_reduce, "ccl all_reduce");
    m.def(
        "all_reduce_low_latency", &all_reduce_low_latency, "low latency all_reduce implementation");
    m.def("all_reduce_caching", &all_reduce_caching, "ccl all_reduce with caching");
    m.def("barrier", &barrier, "barrier");
}
