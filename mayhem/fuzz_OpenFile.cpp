#include <stdint.h>
#include <stdio.h>
#include <climits>

#include <fuzzer/FuzzedDataProvider.h>
#include "scanner.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FuzzedDataProvider provider(data, size);
    std::string name = provider.ConsumeRandomLengthString();

    XL::Positions positions;
    positions.OpenFile(name);

    return 0;
}