#include "lz4.h"
#include "lmdb-js.h"
#include <atomic>

using namespace Napi;

thread_local LZ4_stream_t* Compression::stream = nullptr;
Compression::Compression(const CallbackInfo& info) : ObjectWrap<Compression>(info) {
	unsigned int compressionThreshold = 1000;
	char* dictionary = nullptr;
	size_t dictSize = 0;
	if (info[0].IsObject()) {
		auto dictionaryOption = info[0].As<Object>().Get("dictionary");
		if (!dictionaryOption.IsUndefined()) {
			if (!dictionaryOption.IsTypedArray()) {
				throwError(info.Env(), "Dictionary must be a buffer");
				return;
			}
			napi_get_typedarray_info(info.Env(), dictionaryOption, nullptr, &dictSize, (void**) &dictionary, nullptr, nullptr);
			dictSize = (dictSize >> 3) << 3; // make sure it is word-aligned
		}
		auto thresholdOption = info[0].As<Object>().Get("threshold");
		if (thresholdOption.IsNumber()) {
			compressionThreshold = thresholdOption.As<Number>();
		}
	}
	this->dictionary = this->compressDictionary = dictionary;
	this->dictionarySize = dictSize;
	this->decompressTarget = dictionary + dictSize;
	this->decompressSize = 0;
	this->acceleration = 1;
	this->compressionThreshold = compressionThreshold;
	this->Ref();
	info.This().As<Object>().Set("address", Number::New(info.Env(), (double) (size_t) this));
}
Compression::~Compression() {
}

Napi::Value Compression::setBuffer(const CallbackInfo& info) {
	napi_get_typedarray_info(info.Env(), info[0], nullptr, nullptr, (void**) &this->decompressTarget, nullptr, nullptr);
	this->decompressSize = info[1].As<Number>();
	napi_get_typedarray_info(info.Env(), info[2], nullptr, nullptr, (void**) &this->dictionary, nullptr, nullptr);
	this->dictionarySize = info[3].As<Number>();
	return info.Env().Undefined();
}
extern "C" EXTERN void setCompressionBuffer(double compressionPointer, char* decompressTarget, uint32_t decompressSize, char* dictionary, uint32_t dictSize) {
	Compression *compression = (Compression*) (size_t) compressionPointer;
	compression->dictionary = dictionary;
	compression->decompressTarget = decompressTarget;
	compression->decompressSize = decompressSize;
	compression->dictionarySize = dictSize;
}

void Compression::decompress(MDB_val& data, bool &isValid, bool canAllocate) {
	uint32_t uncompressedLength;
	int compressionHeaderSize;
	uint32_t compressedLength = data.mv_size;
	unsigned char* charData = (unsigned char*) data.mv_data;

	if (charData[0] == 254) {
		uncompressedLength = ((uint32_t)charData[1] << 16) | ((uint32_t)charData[2] << 8) | (uint32_t)charData[3];
		compressionHeaderSize = 4;
	}
	else if (charData[0] == 255) {
		uncompressedLength = ((uint32_t)charData[4] << 24) | ((uint32_t)charData[5] << 16) | ((uint32_t)charData[6] << 8) | (uint32_t)charData[7];
		compressionHeaderSize = 8;
	}
	else {
		fprintf(stderr, "Unknown status byte %u\n", charData[0]);
		if (canAllocate)
			Nan::ThrowError("Unknown status byte");
		isValid = false;
		return;
	}
	data.mv_data = decompressTarget;
	data.mv_size = uncompressedLength;
	//TODO: For larger blocks with known encoding, it might make sense to allocate space for it and use an ExternalString
	//fprintf(stdout, "compressed size %u uncompressedLength %u, first byte %u\n", data.mv_size, uncompressedLength, charData[compressionHeaderSize]);
	if (uncompressedLength > decompressSize) {
		isValid = false;
		return;
	}
	int written = LZ4_decompress_safe_usingDict(
		(char*)charData + compressionHeaderSize, decompressTarget,
		compressedLength - compressionHeaderSize, decompressSize,
		dictionary, dictionarySize);
	//fprintf(stdout, "first uncompressed byte %X %X %X %X %X %X\n", uncompressedData[0], uncompressedData[1], uncompressedData[2], uncompressedData[3], uncompressedData[4], uncompressedData[5]);
	if (written < 0) {
		fprintf(stderr, "Failed to decompress data %u %u bytes:\n", compressionHeaderSize, uncompressedLength);
		for (uint32_t i = 0; i < compressedLength; i++) {
			fprintf(stderr, "%u ", charData[i]);
		}
		if (canAllocate)
			Nan::ThrowError("Failed to decompress data");
		isValid = false;
		return;
	}
	isValid = true;
}

int Compression::compressInstruction(EnvWrap* env, double* compressionAddress) {
	MDB_val value;
	value.mv_data = (void*)((size_t) * (compressionAddress - 1));
	value.mv_size = *(((uint32_t*)compressionAddress) - 3);
	argtokey_callback_t compressedData = compress(&value, nullptr);
	if (compressedData) {
		*(((uint32_t*)compressionAddress) - 3) = value.mv_size;
		*((size_t*)(compressionAddress - 1)) = (size_t)value.mv_data;
		int64_t status = std::atomic_exchange((std::atomic<int64_t>*) compressionAddress, (int64_t) 0);
		if (status == 1 && env) {
			pthread_mutex_lock(env->writingLock);
			pthread_cond_signal(env->writingCond);
			pthread_mutex_unlock(env->writingLock);
			//fprintf(stderr, "sent compression completion signal\n");
		}
		//fprintf(stdout, "compressed to %p %u %u %p\n", value.mv_data, value.mv_size, status, env);
		return 0;
	} else {
		fprintf(stdout, "failed to compress\n");
		return 1;
	}
}

argtokey_callback_t Compression::compress(MDB_val* value, void (*freeValue)(MDB_val&)) {
	size_t dataLength = value->mv_size;
	char* data = (char*)value->mv_data;
	if (value->mv_size < compressionThreshold && !(value->mv_size > 0 && ((uint8_t*)data)[0] >= 250))
		return freeValue; // don't compress if less than threshold (but we must compress if the first byte is the compression indicator)
	bool longSize = dataLength >= 0x1000000;
	int prefixSize = (longSize ? 8 : 4);
	int maxCompressedSize = LZ4_COMPRESSBOUND(dataLength);
	char* compressed = new char[maxCompressedSize + prefixSize];
	//fprintf(stdout, "compressing %u\n", dataLength);
	if (!stream)
		stream = LZ4_createStream();
	LZ4_loadDict(stream, compressDictionary, dictionarySize);
	int compressedSize = LZ4_compress_fast_continue(stream, data, compressed + prefixSize, dataLength, maxCompressedSize, acceleration);
	if (compressedSize > 0) {
		if (freeValue)
			freeValue(*value);
		uint8_t* compressedData = (uint8_t*)compressed;
		if (longSize) {
			compressedData[0] = 255;
			compressedData[2] = (uint8_t)(dataLength >> 40u);
			compressedData[3] = (uint8_t)(dataLength >> 32u);
			compressedData[4] = (uint8_t)(dataLength >> 24u);
			compressedData[5] = (uint8_t)(dataLength >> 16u);
			compressedData[6] = (uint8_t)(dataLength >> 8u);
			compressedData[7] = (uint8_t)dataLength;
		}
		else {
			compressedData[0] = 254;
			compressedData[1] = (uint8_t)(dataLength >> 16u);
			compressedData[2] = (uint8_t)(dataLength >> 8u);
			compressedData[3] = (uint8_t)dataLength;
		}
		value->mv_size = compressedSize + prefixSize;
		value->mv_data = compressed;
		return ([](MDB_val &value) -> void {
			delete[] (char*)value.mv_data;
		});
	}
	else {
		delete[] compressed;
		return nullptr;
	}
}

class CompressionWorker : public AsyncWorker {
  public:
	CompressionWorker(EnvWrap* env, double* compressionAddress, const Function& callback)
	  : AsyncWorker(callback), env(env), compressionAddress(compressionAddress) {}


	void Execute() {
		uint64_t compressionPointer;
		compressionPointer = std::atomic_exchange((std::atomic<int64_t>*) compressionAddress, (int64_t) 2);
		if (compressionPointer > 1) {
			Compression* compression = (Compression*)(size_t) * ((double*)&compressionPointer);
			compression->compressInstruction(env, compressionAddress);
		}
	}
	void OnOK() {
		// don't actually call the callback, no need
	}

  private:
	EnvWrap* env;
	double* compressionAddress;
};

Napi::Value EnvWrap::compress(const CallbackInfo& info) {
	size_t compressionAddress = info[0].As<Number>().Int64Value();
	CompressionWorker* worker = new CompressionWorker(this, (double*) compressionAddress, info[1].As<Function>());
	worker->Queue();
    return info.Env().Undefined();
}

extern "C" EXTERN void compress(double ewPointer, double compressionJSPointer) {
	EnvWrap* ew = (EnvWrap*) (size_t) ewPointer;
	uint64_t compressionPointer;
	double* compressionAddress = (double*) (size_t) compressionJSPointer;
	compressionPointer = std::atomic_exchange((std::atomic<int64_t>*) compressionAddress, (int64_t) 2);
	if (compressionPointer > 1) {
		Compression* compression = (Compression*)(size_t) * ((double*)&compressionPointer);
		compression->compressInstruction(ew, compressionAddress);
	}
}

/*extern "C" EXTERN uint64_t newCompression(char* dictionary, uint32_t dictSize, uint32_t threshold) {
	dictSize = (dictSize >> 3) << 3; // make sure it is word-aligned
	Compression* compression = new Compression();
	if ((size_t) dictionary < 10)
		dictionary= nullptr;
	compression->dictionary = compression->compressDictionary = dictionary;
	compression->dictionarySize = dictSize;
	compression->decompressTarget = dictionary + dictSize;
	compression->decompressSize = 0;
	compression->acceleration = 1;
	compression->compressionThreshold = threshold;
	return (uint64_t) compression;
}*/

void Compression::setupExports(Napi::Env env, Object exports) {
	Function CompressionClass = DefineClass(env, "Compression", {
		Compression::InstanceMethod("setBuffer", &Compression::setBuffer),
	});
	exports.Set("Compression", CompressionClass);
//	compressionTpl->InstanceTemplate()->SetInternalFieldCount(1);
}


// This file contains code from the node-lmdb project
// Copyright (c) 2013-2017 Timur Kristóf
// Copyright (c) 2021 Kristopher Tate
// Licensed to you under the terms of the MIT license
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
