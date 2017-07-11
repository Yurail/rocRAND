// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

/*
Copyright 2010-2011, D. E. Shaw Research.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

* Redistributions of source code must retain the above copyright
  notice, this list of conditions, and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions, and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

* Neither the name of D. E. Shaw Research nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef ROCRAND_RNG_PHILOX4X32_10_H_
#define ROCRAND_RNG_PHILOX4X32_10_H_

#include <algorithm>
#include <hip/hip_runtime.h>

#ifndef FQUALIFIERS
#define FQUALIFIERS __host__ __device__
#endif // FQUALIFIERS

#include <rocrand.h>

#include "philox4x32_10_state.hpp"
#include "generator_type.hpp"
#include "distributions.hpp"

namespace rocrand_philox4x32_10_detail
{
    // HCC
    #ifdef __HIP_DEVICE_COMPILE__
    __device__
    unsigned int mulhilo32(unsigned int x, unsigned int y, unsigned int& z)
    {
        z = __umulhi(x, y);
        return x * y;
    }
    // NVCC
    #elif __CUDA_ARCH__ > 0
    __device__
    unsigned int mulhilo32(unsigned int x, unsigned int y, unsigned int& z)
    {
        z = __umulhi(x, y);
        return x * y;
    }
    #else
    __host__ __device__
    unsigned int mulhilo32(unsigned int x, unsigned int y, unsigned int& z)
    {
        unsigned long long xy =
            static_cast<unsigned long long>(x) * static_cast<unsigned long long>(y);
        z = xy >> 32;
        return static_cast<unsigned int>(xy);
    }
    #endif

    template <
        class Type, class StateType,
        class Generator, class Distribution
    >
    __global__
    void generate_kernel(StateType * states,
                         bool init_states,
                         unsigned long long seed,
                         unsigned long long offset,
                         Type * data, const size_t n,
                         Generator generator,
                         Distribution distribution)
    {
        typedef decltype(distribution(generator.get4(states))) Type4;

        const unsigned int state_id = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;
        unsigned int index = state_id;
        unsigned int stride = hipGridDim_x * hipBlockDim_x;

        // Load or init the state
        StateType state;
        if(init_states)
        {
            generator.init_state(&state, offset, index, seed);
        }
        else
        {
            state = states[state_id];
        }

        // TODO: It's possible to improve performance for situations when
        // generate_poisson_kernel was not called before generate_kernel
        //
        // TODO: We need to check if ordering is so imporant, or if we can
        // skip some random numbers (which increases performance).
        Type4 * data4 = (Type4 *)data;
        while(index < (n/4))
        {
            // Base on state.sub_state calculate correct 4 uint values
            // from the subsequence and pass them to distribution
            data4[index] = distribution(generator.get4(&state));
            // Next position
            index += stride;
        }

        // First work-item saves the tail when n is not a multiple of 4
        auto tail_size = n % 4;
        if(tail_size > 0 && hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x == 0)
        {
            // Base on state.sub_state calculate correct 4 uint values
            // from the subsequence and pass them to distribution
            Type4 result = distribution(generator.get4(&state));
            // Save the tail
            data[n - tail_size] = result.x;
            if(tail_size > 1) data[n - tail_size + 1] = result.y;
            if(tail_size > 2) data[n - tail_size + 2] = result.z;
            // Update state.sub_state
            state.sub_state = (tail_size + state.sub_state) % 4;
        }
        // Save state
        states[state_id] = state;
    }

    template <
        class RealType, class StateType,
        class Generator, class Distribution
    >
    __global__
    void generate_normal_kernel(StateType * states,
                                bool init_states,
                                unsigned long long seed,
                                unsigned long long offset,
                                RealType * data, const size_t n,
                                Generator generator,
                                Distribution distribution)
    {
        // RealTypeX can be float4, double2
        typedef decltype(distribution(generator.get4(states))) RealTypeX;
        // x can be 2 or 4
        const unsigned x = sizeof(RealTypeX) / sizeof(RealType);

        const unsigned int state_id = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;
        unsigned int index = state_id;
        unsigned int stride = hipGridDim_x * hipBlockDim_x;

        // Load or init the state
        StateType state;
        if(init_states)
        {
            generator.init_state(&state, offset, index, seed);
        }
        else
        {
            state = states[state_id];
        }

        // TODO: Improve ordering. Current implementation isn't the best,
        // because box_muller which is used to generate float and double
        // values takes 2 values and generate 2 outputs. Problem occurs
        // when substate is not 0.
        //
        // TODO: Improve performance. For some reason it's ~30 - 40%
        // slower compared to generate_kernel and curandGenerateNormal.
        RealTypeX * dataX = (RealTypeX *)data;
        while(index < (n/x))
        {
            // Base on state.sub_state calculate correct 4 uint values
            // from the subsequence and pass them to distribution
            dataX[index] = distribution(generator.get4(&state));
            // Next position
            index += stride;
        }

        // First work-item saves the tail when n is not a multiple of X
        auto tail_size = n % x;
        if(tail_size > 0 && hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x == 0)
        {
            // Base on state.sub_state calculate correct 4 uint values
            // from the subsequence and pass them to distribution
            RealTypeX result = distribution(generator.get4(&state));
            // Save the tail
            data[n - tail_size] = (&result.x)[0]; // .x
            if(tail_size > 1) data[n - tail_size + 1] = (&result.x)[1]; // .y
            if(tail_size > 2) data[n - tail_size + 2] = (&result.x)[2]; // .z
            // Update state.sub_state
            state.sub_state = (tail_size + state.sub_state) % 4;
        }

        // Save state
        states[state_id] = state;
    }

    // Returns 1 value (runs Philox generation per every 4 calls)
    template<class Generator, class StateType>
    struct generator_state_wrapper
    {
        __host__ __device__
        generator_state_wrapper(Generator generator, StateType state)
            : generator(generator), state(state) {}

        __forceinline__ __host__ __device__
        unsigned int operator()()
        {
            return generator(&state);
        }

        Generator generator;
        StateType state;
    };

    template <
        class StateType,
        class Generator, class Distribution
    >
    __global__
    void generate_poisson_kernel(StateType * states,
                                 bool init_states,
                                 unsigned long long seed,
                                 unsigned long long offset,
                                 unsigned int * data, const size_t n,
                                 Generator generator,
                                 Distribution distribution)
    {
        const unsigned int state_id = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;
        unsigned int index = state_id;
        unsigned int stride = hipGridDim_x * hipBlockDim_x;

        // Load or init the state
        StateType state;
        if(init_states)
        {
            generator.init_state(&state, offset, index, seed);
        }
        else
        {
            state = states[state_id];
        }

        generator_state_wrapper<Generator, StateType> gen(generator, state);

        // TODO: Improve performance.
        while(index < n)
        {
            auto result = distribution(gen);
            data[index] = result;
            index += stride;
        }

        // Save state
        states[state_id] = gen.state;
    }

} // end namespace rocrand_philox4x32_10_detail

class rocrand_philox4x32_10 : public rocrand_generator_type<ROCRAND_RNG_PSEUDO_PHILOX4_32_10>
{
public:
    using base_type = rocrand_generator_type<ROCRAND_RNG_PSEUDO_PHILOX4_32_10>;
    using state_type = base_type::state_type;

    class philox4x32_10_generator
    {
        // Constants from Random123
        // See https://www.deshawresearch.com/resources_random123.html
        static const unsigned int PHILOX_M4x32_0 = 0xD2511F53;
        static const unsigned int PHILOX_M4x32_1 = 0xCD9E8D57;
        static const unsigned int PHILOX_W32_0   = 0x9E3779B9;
        static const unsigned int PHILOX_W32_1   = 0xBB67AE85;

    public:

        __forceinline__ __host__ __device__
        uint operator()(state_type * state)
        {
            uint ret = (&state->result.x)[state->sub_state];
            state->sub_state++;
            if(state->sub_state == 4)
            {
                state->sub_state = 0;
                discard(state);
                state->result = ten_rounds(state->counter, state->key);
            }
            return ret;
        }

        __forceinline__ __host__ __device__
        uint4 get4(state_type * state)
        {
            uint4 ret = state->result;
            discard(state);
            state->result = ten_rounds(state->counter, state->key);
            switch(state->sub_state){
            case 0:
                return ret;
            case 1:
                ret.x = ret.y;
                ret.y = ret.z;
                ret.z = ret.w;
                ret.w = state->result.x;
                break;
            case 2:
                ret.x = ret.z;
                ret.y = ret.w;
                ret.z = state->result.x;
                ret.w = state->result.y;
                break;
            case 3:
                ret.x = ret.w;
                ret.y = state->result.x;
                ret.z = state->result.y;
                ret.w = state->result.z;
                break;
            default:
                return ret;
            }
            return ret;
        }

        // Get uint4 ignoring state's substate
        __forceinline__ __host__ __device__
        uint4 unsafe_get4(state_type * state)
        {
            uint4 ret = state->result;
            state->result = ten_rounds(state->counter, state->key);
            discard(state);
            return ret;
        }

        __forceinline__ __host__ __device__
        void init_state(state_type * state,
                        unsigned long long offset,
                        unsigned long long sequence,
                        unsigned long long seed)
        {
            state->set_seed(seed);
            state->discard_sequence(sequence);
            state->discard(offset);
            state->result = ten_rounds(state->counter, state->key);
        }

        __forceinline__ __host__ __device__
        void discard(state_type * state, unsigned int n)
        {
            state->discard(n);
        }

        __forceinline__ __host__ __device__
        void discard(state_type * state)
        {
            state->discard();
        }

        __forceinline__ __host__ __device__
        void discard_sequence(state_type * state, unsigned int n)
        {
            state->discard_sequence(n);
        }

    private:
        __forceinline__ __host__ __device__
        uint4 single_round(uint4 counter, uint2 key)
        {
            namespace detail = rocrand_philox4x32_10_detail;

            // Source: Random123
            unsigned int hi0;
            unsigned int hi1;
            unsigned int lo0 = detail::mulhilo32(PHILOX_M4x32_0, counter.x, hi0);
            unsigned int lo1 = detail::mulhilo32(PHILOX_M4x32_1, counter.z, hi1);
            return uint4 {
                hi1 ^ counter.y ^ key.x,
                lo1,
                hi0 ^ counter.w ^ key.y,
                lo0
            };
        }

        __forceinline__ __host__ __device__
        uint2 bumpkey(uint2 key)
        {
            key.x += PHILOX_W32_0;
            key.y += PHILOX_W32_1;
            return key;
        }

        __forceinline__ __host__ __device__
        uint4 ten_rounds(uint4 counter, uint2 key)
        {
            counter = single_round(counter, key); key = bumpkey(key); // 1
            counter = single_round(counter, key); key = bumpkey(key); // 2
            counter = single_round(counter, key); key = bumpkey(key); // 3
            counter = single_round(counter, key); key = bumpkey(key); // 4
            counter = single_round(counter, key); key = bumpkey(key); // 5
            counter = single_round(counter, key); key = bumpkey(key); // 6
            counter = single_round(counter, key); key = bumpkey(key); // 7
            counter = single_round(counter, key); key = bumpkey(key); // 8
            counter = single_round(counter, key); key = bumpkey(key); // 9
            return single_round(counter, key);                        // 10
        }
    };

    rocrand_philox4x32_10(unsigned long long seed = 0,
                          unsigned long long offset = 0,
                          hipStream_t stream = 0)
        : base_type(seed, offset, stream),
          m_states_initialized(false), m_states(NULL), m_states_size(1024 * 256)
    {
        auto error = hipMalloc(&m_states, sizeof(state_type) * m_states_size);
        if(error != hipSuccess)
        {
            throw ROCRAND_STATUS_ALLOCATION_FAILED;
        }
    }

    ~rocrand_philox4x32_10()
    {
        hipFree(m_states);
    }

    void reset()
    {
        m_states_initialized = false;
    }

    /// Changes seed to \p seed and resets generator state.
    void set_seed(unsigned long long seed)
    {
        m_seed = seed;
        m_states_initialized = false;
    }

    void set_offset(unsigned long long offset)
    {
        m_offset = offset;
        m_states_initialized = false;
    }

    template<class T, class Distribution = uniform_distribution<T> >
    rocrand_status generate(T * data, size_t data_size,
                            const Distribution& distribution = Distribution())
    {
        #ifdef __HIP_PLATFORM_NVCC__
        const uint32_t threads = 128;
        const uint32_t max_blocks = 128; // 512
        #else
        const uint32_t threads = 256;
        const uint32_t max_blocks = 1024;
        #endif
        const uint32_t blocks = max_blocks;

        namespace detail = rocrand_philox4x32_10_detail;
        hipLaunchKernelGGL(
            HIP_KERNEL_NAME(detail::generate_kernel),
            dim3(blocks), dim3(threads), 0, m_stream,
            m_states, !m_states_initialized, m_seed, m_offset,
            data, data_size, m_generator, distribution
        );
        // Check kernel status
        if(hipPeekAtLastError() != hipSuccess)
            return ROCRAND_STATUS_LAUNCH_FAILURE;

        m_states_initialized = true;
        return ROCRAND_STATUS_SUCCESS;
    }

    template<class T>
    rocrand_status generate_uniform(T * data, size_t n)
    {
        uniform_distribution<T> udistribution;
        return generate(data, n, udistribution);
    }

    template<class T>
    rocrand_status generate_normal(T * data, size_t data_size, T stddev, T mean)
    {
        #ifdef __HIP_PLATFORM_NVCC__
        const uint32_t threads = 128;
        const uint32_t max_blocks = 128; // 512
        #else
        const uint32_t threads = 256;
        const uint32_t max_blocks = 1024;
        #endif
        const uint32_t blocks = max_blocks;

        normal_distribution<T> distribution(mean, stddev);

        namespace detail = rocrand_philox4x32_10_detail;
        hipLaunchKernelGGL(
            HIP_KERNEL_NAME(detail::generate_normal_kernel),
            dim3(blocks), dim3(threads), 0, m_stream,
            m_states, !m_states_initialized, m_seed, m_offset,
            data, data_size, m_generator, distribution
        );
        // Check kernel status
        if(hipPeekAtLastError() != hipSuccess)
            return ROCRAND_STATUS_LAUNCH_FAILURE;

        m_states_initialized = true;
        return ROCRAND_STATUS_SUCCESS;
    }

    template<class T>
    rocrand_status generate_log_normal(T * data, size_t data_size, T stddev, T mean)
    {
        #ifdef __HIP_PLATFORM_NVCC__
        const uint32_t threads = 128;
        const uint32_t max_blocks = 128; // 512
        #else
        const uint32_t threads = 256;
        const uint32_t max_blocks = 1024;
        #endif
        const uint32_t blocks = max_blocks;

        log_normal_distribution<T> distribution(mean, stddev);

        namespace detail = rocrand_philox4x32_10_detail;
        hipLaunchKernelGGL(
            HIP_KERNEL_NAME(detail::generate_normal_kernel),
            dim3(blocks), dim3(threads), 0, m_stream,
            m_states, !m_states_initialized, m_seed, m_offset,
            data, data_size, m_generator, distribution
        );
        // Check kernel status
        if(hipPeekAtLastError() != hipSuccess)
            return ROCRAND_STATUS_LAUNCH_FAILURE;

        m_states_initialized = true;
        return ROCRAND_STATUS_SUCCESS;
    }

    rocrand_status generate_poisson(unsigned int * data, size_t data_size, double lambda)
    {
        #ifdef __HIP_PLATFORM_NVCC__
        const uint32_t threads = 128;
        const uint32_t max_blocks = 128; // 512
        #else
        const uint32_t threads = 256;
        const uint32_t max_blocks = 1024;
        #endif
        const uint32_t blocks = max_blocks;

        poisson_distribution<unsigned int> distribution(lambda);

        namespace detail = rocrand_philox4x32_10_detail;
        hipLaunchKernelGGL(
            HIP_KERNEL_NAME(detail::generate_poisson_kernel),
            dim3(blocks), dim3(threads), 0, m_stream,
            m_states, !m_states_initialized, m_seed, m_offset,
            data, data_size, m_generator, distribution
        );
        // Check kernel status
        if(hipPeekAtLastError() != hipSuccess)
            return ROCRAND_STATUS_LAUNCH_FAILURE;

        m_states_initialized = true;
        return ROCRAND_STATUS_SUCCESS;
    }

private:
    bool m_states_initialized;
    state_type * m_states;
    size_t m_states_size;

    // m_seed from base_type
    // m_offset from base_type
    philox4x32_10_generator m_generator;
};

#endif // ROCRAND_RNG_PHILOX4X32_10_H_
