#include <stdint.h>
#include <stdio.h>
#include <climits>

#include <fuzzer/FuzzedDataProvider.h>

#include <AnKi/Math/Functions.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	FuzzedDataProvider provider(data, size);

    anki::F64 a = provider.ConsumeFloatingPoint<anki::F64>();
    anki::F64 sina = provider.ConsumeFloatingPoint<anki::F64>();
    anki::F64 cosa = provider.ConsumeFloatingPoint<anki::F64>();

    anki::sinCos(a, sina, cosa);

	return 0;
}