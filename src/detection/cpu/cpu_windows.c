#include "cpu.h"
#include "detection/temps/temps_windows.h"
#include "util/windows/registry.h"
#include "util/mallocHelper.h"
#include "util/smbiosHelper.h"

// 7.5
typedef struct FFSmbiosProcessorInfo
{
    FFSmbiosHeader Header;

    uint8_t SocketDesignation; // string
    uint8_t ProcessorType; // enum
    uint8_t ProcessorFamily; // enum
    uint8_t ProcessorManufacturer; // string
    uint64_t ProcessorID; // varies
    uint8_t ProcessorVersion; // string
    uint8_t Voltage; // varies
    uint16_t ExternalClock; // varies
    uint16_t MaxSpeed; // varies
    uint16_t CurrentSpeed; // varies
    uint8_t Status; // varies
    uint8_t ProcessorUpgrade; // enum

    // 2.1+
    uint16_t L1CacheHandle; // varies
    uint16_t L2CacheHandle; // varies
    uint16_t L3CacheHandle; // varies

    // 2.3+
    uint8_t SerialNumber; // string
    uint8_t AssertTag; // string
    uint8_t PartNumber; // string

    // 2.5+
    uint8_t CoreCount; // varies
    uint8_t CoreEnabled; // varies
    uint8_t ThreadCount; // varies
    uint16_t ProcessorCharacteristics; // bit field

    // 2.6+
    uint16_t ProcessorFamily2; // enum

    // 3.0+
    uint16_t CoreCount2; // varies
    uint16_t CoreEnabled2; // varies
    uint16_t ThreadCount2; // varies

    // 3.6+
    uint16_t ThreadEnabled; // varies
} FFSmbiosProcessorInfo;

static const char* detectMaxSpeedBySmbios(FFCPUResult* cpu)
{
    const FFSmbiosProcessorInfo* data = (const FFSmbiosProcessorInfo*) (*ffGetSmbiosHeaderTable())[FF_SMBIOS_TYPE_PROCESSOR_INFO];

    if (!data)
        return "Processor information is not found in SMBIOS data";

    while (data->ProcessorType != 0x03 /*Central Processor*/ || (data->Status & 0b00000111) != 1 /*Enabled*/)
    {
        data = (const FFSmbiosProcessorInfo*) ffSmbiosNextEntry(&data->Header);
        if (data->Header.Type != FF_SMBIOS_TYPE_PROCESSOR_INFO)
            return "No active CPU is found in SMBIOS data";
    }

    double speed = data->MaxSpeed / 1000.0;
    // Sometimes SMBIOS reports invalid value. We assume that max speed is small than 2x of base
    if (speed < cpu->frequencyBase || speed > cpu->frequencyBase * 2)
        return "Possible invalid CPU max speed in SMBIOS data. See #800";

    cpu->frequencyMax = speed;

    return NULL;
}

static const char* detectNCores(FFCPUResult* cpu)
{
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationAll, NULL, &length);
    if (length == 0)
        return "GetLogicalProcessorInformationEx(RelationAll, NULL, &length) failed";

    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* FF_AUTO_FREE
        pProcessorInfo = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)malloc(length);

    if (!pProcessorInfo || !GetLogicalProcessorInformationEx(RelationAll, pProcessorInfo, &length))
        return "GetLogicalProcessorInformationEx(RelationAll, pProcessorInfo, &length) failed";

    for(
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* ptr = pProcessorInfo;
        (uint8_t*)ptr < ((uint8_t*)pProcessorInfo) + length;
        ptr = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(((uint8_t*)ptr) + ptr->Size)
    )
    {
        if(ptr->Relationship == RelationProcessorCore)
            ++cpu->coresPhysical;
        else if(ptr->Relationship == RelationGroup)
        {
            for (uint32_t index = 0; index < ptr->Group.ActiveGroupCount; ++index)
            {
                cpu->coresOnline += ptr->Group.GroupInfo[index].ActiveProcessorCount;
                cpu->coresLogical += ptr->Group.GroupInfo[index].MaximumProcessorCount;
            }
        }
    }

    return NULL;
}

static const char* detectByRegistry(FFCPUResult* cpu)
{
    FF_HKEY_AUTO_DESTROY hKey = NULL;
    if(!ffRegOpenKeyForRead(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", &hKey, NULL))
        return "ffRegOpenKeyForRead(HKEY_LOCAL_MACHINE, L\"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0\", &hKey, NULL) failed";

    {
        uint32_t mhz;
        if(ffRegReadUint(hKey, L"~MHz", &mhz, NULL))
            cpu->frequencyBase = mhz / 1000.0;
    }

    ffRegReadStrbuf(hKey, L"ProcessorNameString", &cpu->name, NULL);
    ffRegReadStrbuf(hKey, L"VendorIdentifier", &cpu->vendor, NULL);

    if (cpu->coresLogical == 0)
    {
        DWORD cores;
        if (RegQueryInfoKeyW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor", NULL, NULL, &cores, NULL, NULL, NULL, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
            cpu->coresOnline = cpu->coresPhysical = cpu->coresLogical = (uint16_t) cores;
    }

    return NULL;
}

const char* ffDetectCPUImpl(const FFCPUOptions* options, FFCPUResult* cpu)
{
    detectNCores(cpu);

    const char* error = detectByRegistry(cpu);
    if (error)
        return error;

    detectMaxSpeedBySmbios(cpu);

    if(options->temp)
        ffDetectSmbiosTemp(&cpu->temperature, NULL);

    return NULL;
}
