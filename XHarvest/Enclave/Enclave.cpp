/*
 * Copyright (C) 2011-2018 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "Enclave_t.h"
#include "sgx_nvme.h"
#include "sgx_ftl.h"

#include <sgx_trts.h>
#include <stdio.h>

/*

static uint64_t rand(void)
{
		uint64_t num = 0;
		sgx_read_rand((unsigned char*)&num, sizeof(uint32_t));
		return num;
}

static inline uint64_t rdtsc(void)
{
		uint32_t hi, lo;
		__asm__ __volatile__("" : : : "memory");
		__asm__ __volatile__("rdtsc"
												 : "=a"(lo), "=d"(hi));
		__asm__ __volatile__("" : : : "memory");

		return (uint64_t(hi) << 32) | uint64_t(lo);
}

uint64_t clock_to_nsec(uint64_t begin, uint64_t end) {
	double clock_diff; //  这里做一些简单的单调性处理, 防止 end < begin.
	if (end < begin) return (0);
	clock_diff = (double)(end - begin);
	return ((uint64_t)(clock_diff);
}



void ftl_evaluation(struct ssd* ssd, int SRmode, int RWmode) {
		NvmeRequest *req = (NvmeRequest *)(malloc(sizeof (NvmeRequest)));
		uint64_t SSDsize = ssd->sp.tt_pgs * 50 / 100;
		uint64_t evaRound = 10000000;
		uint64_t nowRound = 0;

		uint64_t MaxIOSize = 31;
		uint64_t IOSize = 1;

		uint64_t start = rdtsc();

		//    SRmode:
		//    * 0: sequential
		//    * 1: random
		//    * 2: skipping


		//    RWmode:
		//    * 0: read
		//    * 1: write
		//    * 2: mixed(true random)
		//    * 3: mixed(pesudo random)


		int skipSize = 16;

		while (nowRound++ < evaRound) {
				switch (SRmode) {
						case 0:
								req->slba = (nowRound * IOSize) % SSDsize;
								req->nlb = IOSize;
								if (req->slba + req->nlb > SSDsize) {
										req->nlb = SSDsize - req->slba;
								}
								break;
						case 1:
								req->slba = rand() % (SSDsize - 1);
								req->nlb = IOSize;
								if (req->slba + req->nlb > SSDsize) {
										req->nlb = SSDsize - req->slba;
								}
								break;
						case 2:
								req->slba = (nowRound * skipSize) % SSDsize;
								req->nlb = IOSize;
								if (req->slba + req->nlb > SSDsize) {
										req->nlb = SSDsize - req->slba;
								}
								break;
						default:
								break;
				}

				req->slba *= 8;
				req->nlb *= 8;
				req->stime = 0;

				switch (RWmode) {
						case 0:
								ssd_read(ssd, req);
								break;
						case 1:
								ssd_write(ssd, req);
								break;
						case 2:
								if (rand() % 2) ssd_read(ssd, req);
								else ssd_write(ssd, req);
								break;
						case 3:
								if (nowRound & 1) ssd_read(ssd, req);
								else ssd_write(ssd, req);
								break;
						default:
								break;
				}
		}

		uint64_t end = rdtsc();

		char buf[100];
		snprintf(buf, 100, "ftl_evaluation: %lu nowRound: %lu 233\n", end - start, nowRound);
		ocall_print_string((const char *)buf);
}
*/

void ecall_hello_world(int SRmode, int RWmode)
{
	ocall_print_string("Hello World\n");

	// struct ssd *ssd = (struct ssd *)(malloc(sizeof (struct ssd)));
	// ssd_init(ssd);

	// ftl_evaluation(ssd, SRmode, RWmode);

	ocall_print_string("Hello World\n");
}

void ecall_SGX_thread(void *data) {
	ocall_print_string("Enter SSD thread in Intel SGX environment\n");

	ftl_thread(data);
}
