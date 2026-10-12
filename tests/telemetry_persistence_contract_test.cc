#include "storage/DeviceRepository.h"
#include "storage/TelemetryRepository.h"
#include "storage/TelemetryWriter.h"

#include <cassert>
#include <iostream>
#include <stdexcept>

int main()
{
    bool rejectedNullClient = false;
    try
    {
        labscope::storage::TelemetryRepository repository(nullptr);
    }
    catch (const std::invalid_argument &)
    {
        rejectedNullClient = true;
    }
    assert(rejectedNullClient);

    rejectedNullClient = false;
    try
    {
        labscope::storage::DeviceRepository repository(nullptr);
    }
    catch (const std::invalid_argument &)
    {
        rejectedNullClient = true;
    }
    assert(rejectedNullClient);

    rejectedNullClient = false;
    try
    {
        labscope::storage::TelemetryWriter writer(nullptr);
    }
    catch (const std::invalid_argument &)
    {
        rejectedNullClient = true;
    }
    assert(rejectedNullClient);

    std::cout << "persistence repository constructor contracts passed\n";
    return 0;
}
