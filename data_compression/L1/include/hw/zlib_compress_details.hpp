/*
 * (c) Copyright 2020 Xilinx, Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#ifndef _XFCOMPRESSION_ZLIB_COMPRESS_DETAILS_HPP_
#define _XFCOMPRESSION_ZLIB_COMPRESS_DETAILS_HPP_

/**
 * @file zlib_compress_details.hpp
 * @brief Header for modules used in ZLIB compression kernel.
 *
 * This file is part of Vitis Data Compression Library.
 */

#include <ap_int.h>
#include <fstream>
#include <iostream>
#include <stdlib.h>
#include <string>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "zlib_specs.hpp"
#include "lz_optional.hpp"
#include "lz_compress.hpp"
#include "huffman_treegen.hpp"
#include "huffman_encoder.hpp"
#include "mm2s.hpp"
#include "s2mm.hpp"
#include "stream_downsizer.hpp"
#include "stream_upsizer.hpp"

namespace xf {
namespace compression {
namespace details {

template <int SLAVES>
void streamEosDistributor(hls::stream<bool>& inStream, hls::stream<bool> outStream[SLAVES]) {
    do {
        bool i = inStream.read();
        for (int n = 0; n < SLAVES; n++) outStream[n] << i;
        if (i == 0) break;
    } while (1);
}

template <int SLAVES>
void streamSizeDistributor(hls::stream<uint32_t>& inStream, hls::stream<uint32_t> outStream[SLAVES]) {
    do {
        uint32_t i = inStream.read();
        for (int n = 0; n < SLAVES; n++) outStream[n] << i;
        if (i == 0) break;
    } while (1);
}

template <int DATAWIDTH = 512, int BURST_SIZE = 16, int MAX_BLOCK_SIZE = 64 * 1024>
void mm2sZlib(const ap_uint<DATAWIDTH>* in,
              hls::stream<ap_uint<DATAWIDTH> >& outStream,
              hls::stream<uint32_t>& outSizeStream,
              uint32_t input_size) {
    const int c_byteSize = 8;
    const int c_wordSize = DATAWIDTH / c_byteSize;
    const int block_length = MAX_BLOCK_SIZE;
    const int no_blocks = (input_size - 1) / block_length + 1;
    uint32_t readBlockSize = 0, rIdx = 0;

    for (uint32_t i = 0; i < no_blocks; i++) {
        uint32_t transferSize = block_length;
        if (readBlockSize + block_length > input_size) transferSize = input_size - readBlockSize;
        outSizeStream << transferSize;
        readBlockSize += transferSize;
        uint32_t readSize = 0;
        bool is_pending = true;
        while (is_pending) {
            uint32_t pendingBytes = (transferSize > readSize) ? (transferSize - readSize) : 0;
            is_pending = (pendingBytes > 0) ? true : false;
            if (pendingBytes) {
                uint32_t pendingWords = (pendingBytes - 1) / c_wordSize + 1;
                uint32_t burstSize = (pendingWords > BURST_SIZE) ? BURST_SIZE : pendingWords;
            gmem_read:
                for (uint32_t midx = 0; midx < burstSize; midx++) {
                    outStream << in[rIdx + midx];
                }
                rIdx += burstSize;
                readSize += burstSize * c_wordSize;
            }
        }
    }
    outSizeStream << 0;
}

template <int IN_DATAWIDTH, int OUT_DATAWIDTH>
void streamDownSizerZlib(hls::stream<ap_uint<IN_DATAWIDTH> >& inStream,
                         hls::stream<uint32_t>& inSizeStream,
                         hls::stream<ap_uint<OUT_DATAWIDTH> >& outStream,
                         hls::stream<uint32_t>& outSizeStream) {
    const int c_byteWidth = 8;
    const int c_inputWord = IN_DATAWIDTH / c_byteWidth;
    const int c_outWord = OUT_DATAWIDTH / c_byteWidth;
    const int factor = c_inputWord / c_outWord;
    ap_uint<IN_DATAWIDTH> inBuffer = 0;

downsizer_top:
    for (uint32_t inSize = inSizeStream.read(); inSize != 0; inSize = inSizeStream.read()) {
        outSizeStream << inSize;
        uint32_t outSizeV = (inSize - 1) / c_outWord + 1;
    downsizer_assign:
        for (uint32_t itr = 0; itr < outSizeV; itr++) {
#pragma HLS PIPELINE II = 1
            int idx = itr % factor;
            if (idx == 0) inBuffer = inStream.read();
            ap_uint<OUT_DATAWIDTH> tmpValue = inBuffer.range((idx + 1) * OUT_DATAWIDTH - 1, idx * OUT_DATAWIDTH);
            outStream << tmpValue;
        }
    }
    outSizeStream << 0;
}

template <int IN_WIDTH, int OUT_WIDTH>
void upsizerZlib(hls::stream<ap_uint<IN_WIDTH> >& inStream,
                 hls::stream<bool>& inStreamEos,
                 hls::stream<bool>& inFileEos,
                 hls::stream<bool>& outFileEos,
                 hls::stream<ap_uint<OUT_WIDTH> >& outStream,
                 hls::stream<bool>& outStreamEos) {
    const int c_byteWidth = 8;
    const int c_upsizeFactor = OUT_WIDTH / IN_WIDTH;
    const int c_wordSize = OUT_WIDTH / c_byteWidth;

    while (1) {
        bool eosFile = inFileEos.read();
        outFileEos << eosFile;
        if (eosFile == true) break;

        ap_uint<OUT_WIDTH> outBuffer = 0;
        uint32_t byteIdx = 0;
        bool eos_flag = false;
    stream_upsizer:
        do {
#pragma HLS PIPELINE II = 1
            if (byteIdx == c_upsizeFactor) {
                outStream << outBuffer;
                outStreamEos << false;
                byteIdx = 0;
            }
            ap_uint<IN_WIDTH> inValue = inStream.read();
            eos_flag = inStreamEos.read();
            outBuffer.range((byteIdx + 1) * IN_WIDTH - 1, byteIdx * IN_WIDTH) = inValue;
            byteIdx++;
        } while (eos_flag == false);

        if (byteIdx) {
            outStream << outBuffer;
            outStreamEos << 0;
        }

        outStream << 0;
        outStreamEos << 1;
    }
}

template <int DATAWIDTH = 512, int BURST_SIZE = 16, int MAX_BLOCK_SIZE = 64 * 1024>
void s2mmZlib(ap_uint<DATAWIDTH>* out,
              hls::stream<ap_uint<DATAWIDTH> >& inStream,
              hls::stream<bool>& inStreamEos,
              hls::stream<bool>& inFileEos,
              hls::stream<uint32_t>& inSizeStream,
              uint32_t* compressedSize) {
    const int c_byteSize = 8;
    const int c_wordSize = DATAWIDTH / c_byteSize;
    const int block_length = MAX_BLOCK_SIZE;
    const int blkStride = block_length / c_wordSize;

    uint32_t blkIdx = 0, blkCntr = 0;

    while (1) {
        bool eosFile = inFileEos.read();
        if (eosFile == true) break;

        bool eos = false;
        ap_uint<DATAWIDTH> dummy = 0;
    s2mm_eos_simple:
        for (int j = 0; eos == false; j += BURST_SIZE) {
            for (int i = 0; i < BURST_SIZE; i++) {
#pragma HLS PIPELINE II = 1
                ap_uint<DATAWIDTH> tmp = (eos == true) ? dummy : inStream.read();
                bool eos_tmp = (eos == true) ? true : inStreamEos.read();
                out[blkIdx + j + i] = tmp;
                eos = eos_tmp;
            }
        }
        blkIdx += blkStride;
        blkCntr++;
    }

    for (uint32_t j = 0; j < blkCntr; j++) {
        compressedSize[j] = inSizeStream.read();
    }
}

template <int OUT_DWIDTH>
void zlibCompressStreamingPacker(hls::stream<ap_uint<OUT_DWIDTH> >& inStream,
                                 hls::stream<ap_uint<OUT_DWIDTH> >& outStream,
                                 hls::stream<bool>& inStreamEos,
                                 hls::stream<bool>& outStreamEos,
                                 hls::stream<bool>& inFileEos,
                                 hls::stream<uint32_t>& inputCmpBlockSizeStream,
                                 hls::stream<uint32_t>& outputTotalSizeStream,
                                 hls::stream<ap_uint<32> >& inNumBlockStream,
                                 hls::stream<bool>& outNumBlockStreamEos) {
    const int c_parallelByte = OUT_DWIDTH / 8;
    for (ap_uint<32> no_blocks = inNumBlockStream.read(); no_blocks != 0; no_blocks = inNumBlockStream.read()) {
        uint32_t size = 0, lbuf_idx = 0, prodLbuf = 0;
        outNumBlockStreamEos << 0;
        outStream << 0x0178;
        outStreamEos << 0;
        size += 2;
        ap_uint<3 * OUT_DWIDTH> lcl_buffer = 0;
        for (uint32_t i = 0; i < no_blocks; i++) {
            inFileEos.read();
            uint32_t size_read = 0;
            // One-time multiplication calculation for index
            prodLbuf = lbuf_idx * 8;
            if (i == no_blocks - 1) {
                ap_uint<OUT_DWIDTH> val = inStream.read();
                if (inStreamEos.read()) {
                    continue;
                }
                val |= (ap_uint<OUT_DWIDTH>)1;
                size_read += 2;
                lcl_buffer.range(prodLbuf + OUT_DWIDTH - 1, prodLbuf) = val;
                lbuf_idx += 2;
                prodLbuf = lbuf_idx * 8;
            }

        loop_aligned:
            for (bool eos = inStreamEos.read(); eos == 0; eos = inStreamEos.read()) {
#pragma HLS PIPELINE II = 1
                // Reading Input Data
                lcl_buffer.range(prodLbuf + OUT_DWIDTH - 1, prodLbuf) = inStream.read();
                //                bool tempEos = inStreamEos.read();

                // Writing output into memory
                outStream << lcl_buffer.range(OUT_DWIDTH - 1, 0);
                outStreamEos << 0;
                // Shifting by Global datawidth
                lcl_buffer >>= OUT_DWIDTH;
                size_read += 2;
            }

            uint32_t blockSize = inputCmpBlockSizeStream.read();
            size += blockSize;

            // Calculation for byte processing
            uint32_t leftBytes = blockSize - size_read;

            // Left bytes from each block
            lcl_buffer.range(prodLbuf + OUT_DWIDTH - 1, prodLbuf) = inStream.read();
            lbuf_idx += leftBytes;

            // Check for PARALLEL BYTE data and write to output stream
            if (lbuf_idx >= c_parallelByte) {
                outStream << lcl_buffer.range(OUT_DWIDTH - 1, 0);
                outStreamEos << 0;
                lcl_buffer >>= OUT_DWIDTH;
                lbuf_idx -= c_parallelByte;
            }
        }

        // Left Over Data Handling
        if (lbuf_idx) {
            lcl_buffer.range(OUT_DWIDTH - 1, (lbuf_idx * 8)) = 0;
            outStream << lcl_buffer.range(OUT_DWIDTH - 1, 0);
            outStreamEos << 0;
            lbuf_idx = 0;
        }

        outStream << 0x0001;
        outStreamEos << 0;
        outStream << 0xff00;
        outStreamEos << 0;
        outStream << 0x00ff;
        outStreamEos << 0;
        outStream << 0;
        outStreamEos << 0;
        outStream << 0;
        outStreamEos << 0;
        outStream << 0;
        outStreamEos << 1;
        size += 12;
        outputTotalSizeStream << size;
    }
    inFileEos.read();
    outNumBlockStreamEos << 1;
}

template <int NUM_BLOCK = 4>
void zlibTreegenScheduler(hls::stream<uint32_t> lz77InTree[NUM_BLOCK],
                          hls::stream<bool> lz77TreeBlockEos[NUM_BLOCK],
                          hls::stream<uint32_t>& lz77OutTree,
                          hls::stream<bool>& lz77OutTreeEos,
                          hls::stream<uint8_t>& outIdxNum) {
    constexpr int c_litDistCodeCnt = 286 + 30;
    ap_uint<NUM_BLOCK> is_pending;
    bool eos_tmp[NUM_BLOCK];
    for (uint8_t i = 0; i < NUM_BLOCK; i++) {
        is_pending.range(i, i) = 1;
        eos_tmp[i] = false;
    }
    while (is_pending) {
        for (uint8_t i = 0; i < NUM_BLOCK; i++) {
            bool eos = eos_tmp[i] ? eos_tmp[i] : lz77TreeBlockEos[i].read();
            is_pending.range(i, i) = eos ? 0 : 1;
            eos_tmp[i] = eos;
            if (!eos) {
                outIdxNum << i;
                lz77OutTreeEos << 0;
                for (uint16_t j = 0; j < c_litDistCodeCnt; j++) {
                    uint32_t tmpVal = lz77InTree[i].read();
                    lz77OutTree << tmpVal;
                }
            }
        }
    }
    lz77OutTreeEos << 1;
    outIdxNum << 0xFF;
}

template <int NUM_BLOCK = 4>
void zlibTreegenDistributor(hls::stream<uint16_t> StreamCode[NUM_BLOCK],
                            hls::stream<uint8_t> StreamSize[NUM_BLOCK],
                            hls::stream<uint16_t>& streamSerialCode,
                            hls::stream<uint8_t>& streamSerialSize,
                            hls::stream<uint8_t>& inIdxNum) {
    constexpr int c_litDistCodeCnt = 286 + 30;
    for (uint8_t i = inIdxNum.read(); i != 0xFF; i = inIdxNum.read()) {
        for (uint16_t j = 0; j < c_litDistCodeCnt; j++) {
            StreamSize[i] << streamSerialSize.read();
            StreamCode[i] << streamSerialCode.read();
        }
        for (uint16_t inSize = streamSerialSize.read(); inSize != 0; inSize = streamSerialSize.read()) {
            StreamSize[i] << inSize;
            StreamCode[i] << streamSerialCode.read();
        }
        StreamSize[i] << 0;
    }
}

template <int NUM_BLOCK = 4>
void zlibTreegenStreamWrapper(hls::stream<uint32_t> lz77Tree[NUM_BLOCK],
                              hls::stream<bool> lz77TreeBlockEos[NUM_BLOCK],
                              hls::stream<uint16_t> StreamCode[NUM_BLOCK],
                              hls::stream<uint8_t> StreamSize[NUM_BLOCK]) {
#pragma HLS dataflow
    constexpr int c_litDistCodeCnt = 286 + 30;
    constexpr int c_depthlitDistStream = NUM_BLOCK * c_litDistCodeCnt;
    hls::stream<uint32_t> lz77SerialTree("lz77SerialTree");
    hls::stream<bool> lz77SerialTreeEos("lz77SerialTreeEos");
    hls::stream<uint16_t> streamSerialCode("streamSerialCode");
    hls::stream<uint8_t> streamSerialSize("streamSerialSize");
    hls::stream<uint8_t> idxNum("idxNum");
#pragma HLS STREAM variable = lz77SerialTree depth = c_depthlitDistStream
#pragma HLS STREAM variable = streamSerialCode depth = 32
#pragma HLS STREAM variable = streamSerialSize depth = 32
#pragma HLS STREAM variable = idxNum depth = 32

    zlibTreegenScheduler<NUM_BLOCK>(lz77Tree, lz77TreeBlockEos, lz77SerialTree, lz77SerialTreeEos, idxNum);
    zlibTreegenStream(lz77SerialTree, lz77SerialTreeEos, streamSerialCode, streamSerialSize);
    zlibTreegenDistributor<NUM_BLOCK>(StreamCode, StreamSize, streamSerialCode, streamSerialSize, idxNum);
}

} // End namespace details
} // End namespace compression
} // End namespace xf

#endif // _XFCOMPRESSION_ZLIB_COMPRESS_DETAILS_HPP_
