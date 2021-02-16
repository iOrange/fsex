// 2021, Sergii 'iOrange' Kudlai
// Usage: fsex.exe path/to/base.tntFolder output/folder/path

#include <iostream>
#include <vector>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

#define scast static_cast
#define rcast reinterpret_cast


#include <Windows.h>

#define ZLIB_WINAPI 1
#include "zlib.h"
#pragma comment(lib, "zlibstat.lib")

#pragma pack(push, 1)
struct FSPakHeader {        // 22 bytes
    uint32_t    magic;      // 0x06054b50
    uint16_t    diskNum;
    uint16_t    centralDisk;
    uint16_t    numCentralRecordsHere;
    uint16_t    numCentralRecordsTotal;
    uint32_t    centralDirSize;
    uint32_t    centralDirOffset;
    uint16_t    commentLen;
};

struct FSPakFileRecord {    // 46 bytes
    uint32_t    magic;      // 0x02014b50
    uint16_t    verMade;
    uint16_t    verMinimum;
    uint16_t    flags;
    uint16_t    compression;
    uint16_t    lastModTime;
    uint16_t    lastModDate;
    uint32_t    checkSum;
    uint32_t    sizeCompressed;
    uint32_t    sizeUncompressed;
    uint16_t    nameLength;
    uint16_t    extraFieldLen;
    uint16_t    commentLen;
    uint16_t    disk;
    uint16_t    intArrtibs;
    uint32_t    exAttribs;
    uint32_t    relOffset;
};

struct FSPakLocalFileHeader {   // 30 bytes
    uint32_t    magic;          // 0x04034b50
    uint16_t    verMinimum;
    uint16_t    flags;
    uint16_t    compression;
    uint16_t    lastModTime;
    uint16_t    lastModDate;
    uint32_t    checkSum;
    uint32_t    sizeCompressed;
    uint32_t    sizeUncompressed;
    uint16_t    nameLength;
    uint16_t    extraFieldLen;
};
#pragma pack(pop)

static_assert(sizeof(FSPakHeader) == 22);
static_assert(sizeof(FSPakFileRecord) == 46);
static_assert(sizeof(FSPakLocalFileHeader) == 30);


struct MappedWinFile {
    HANDLE          hFile;
    HANDLE          hMemory;
    size_t          size;
    const uint8_t*  ptr;
};

// WinApi is weird and sometimes can return you a null handle instead of invalid
constexpr bool WinHandleValid(HANDLE h) {
    return h && INVALID_HANDLE_VALUE != h;
}

inline void WinHandleClose(HANDLE& h) {
    if (WinHandleValid(h)) {
        ::CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
}

static bool MapWinFile(const fs::path& path, MappedWinFile& outFile) {
    bool result = false;

    HANDLE hFile, hMemory = INVALID_HANDLE_VALUE;
    hFile = ::CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (WinHandleValid(hFile)) {
        LARGE_INTEGER length;
        ::GetFileSizeEx(hFile, &length);

        hMemory = ::CreateFileMappingW(hFile, nullptr, PAGE_READONLY, length.HighPart, length.LowPart, nullptr);
        if (WinHandleValid(hMemory)) {
            const uint8_t* data = rcast<uint8_t*>(::MapViewOfFile(hMemory, FILE_MAP_READ, 0, 0, length.QuadPart));
            if (data) {
                outFile.hFile = hFile;
                outFile.hMemory = hMemory;
                outFile.size = length.QuadPart;
                outFile.ptr = data;

                result = true;
            }
        }
    }

    if (!result) {
        WinHandleClose(hMemory);
        WinHandleClose(hFile);
    }

    return result;
}

static void UnmapWinFile(MappedWinFile& file) {
    if (file.ptr) {
        ::UnmapViewOfFile(file.ptr);
        file.ptr = nullptr;
    }

    WinHandleClose(file.hMemory);
    WinHandleClose(file.hFile);

    file.size = 0;
}

static bool DumpToWinFile(const fs::path& path, const void* data, const size_t dataLength) {
    bool result = false;

    HANDLE hFile = ::CreateFileW(path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (WinHandleValid(hFile)) {
        const DWORD bytesToWrite = scast<DWORD>(dataLength);
        DWORD bytesWritten = 0;
        result = ::WriteFile(hFile, data, bytesToWrite, &bytesWritten, nullptr) && (bytesWritten == bytesToWrite);
        ::CloseHandle(hFile);
    }

    return result;
}

union Byte4 {
    uint32_t    u;
    struct {
        uint8_t b0;
        uint8_t b1;
        uint8_t b2;
        uint8_t b3;
    };
};

static void fsDecyper(void* data, const size_t dataSize, uint32_t& fullKey) {
    uint8_t* bytes = rcast<uint8_t*>(data);
    Byte4 b4;
    size_t i = 0;
    for (size_t i = 0; i < dataSize; ++i) {
        fullKey = (0x1D * fullKey + 0x1B) % 0x72EBCAFE;
        b4.u = fullKey;
        bytes[i] = b4.b0 ^ b4.b1 ^ b4.b2 ^ b4.b3 ^ bytes[i];
    };
}

struct FileInfo {
    size_t offset;
    size_t keyRehashCounter;
    std::string name;
};

static const uint32_t kDataKeyInit = 0xa2a2a2a2;    // can be compile-time constant, as it depends on the game pack file name

int wmain(int argc, wchar_t** argv) {
    fs::path inputPack(argv[1]);
    fs::path outputPath(argv[2]);

    uint32_t fullKey = kDataKeyInit;

    MappedWinFile file;
    if (MapWinFile(inputPack, file)) {
        FSPakHeader hdr = *rcast<const FSPakHeader*>(file.ptr + (file.size - sizeof(FSPakHeader)));
        fsDecyper(&hdr, sizeof(hdr), fullKey);

        size_t keyRehashCounter = hdr.centralDirSize + sizeof(FSPakHeader);

        std::vector<FileInfo> files;
        files.reserve(hdr.numCentralRecordsHere);

        // first - read through toc and collect files
        char tmp[MAX_PATH] = {};
        const uint8_t* tocPtr = file.ptr + hdr.centralDirOffset;
        for (size_t i = 0; i < hdr.numCentralRecordsHere; ++i) {
            FSPakFileRecord fileRec = *rcast<const FSPakFileRecord*>(tocPtr);
            fsDecyper(&fileRec, sizeof(fileRec), fullKey);

            tocPtr += sizeof(fileRec);
            memcpy(tmp, tocPtr, fileRec.nameLength);
            tocPtr += fileRec.nameLength;
            tmp[fileRec.nameLength] = 0;
            fsDecyper(tmp, fileRec.nameLength, fullKey);

            if (fileRec.sizeUncompressed != 0) {
                FileInfo fi = {};
                fi.offset = fileRec.relOffset;
                fi.keyRehashCounter = keyRehashCounter;
                fi.name = tmp;

                files.push_back(fi);
            }

            keyRehashCounter += fileRec.nameLength + sizeof(FSPakLocalFileHeader);

            const size_t keyRehashTimes = fileRec.extraFieldLen + fileRec.commentLen;
            for (size_t j = 0; j < keyRehashTimes; ++j) {
                fullKey = (0x1D * fullKey + 0x1B) % 0x72EBCAFE;
            }
        }

        // now unpack files
        for (const auto& fi : files) {
            fullKey = kDataKeyInit;
            for (size_t j = 0; j < fi.keyRehashCounter; ++j) {
                fullKey = (0x1D * fullKey + 0x1B) % 0x72EBCAFE;
            }

            FSPakLocalFileHeader localHeader = *rcast<const FSPakLocalFileHeader*>(file.ptr + fi.offset);
            fsDecyper(&localHeader, sizeof(localHeader), fullKey);

            std::cout << "Extracting " << fi.name << " of size " << localHeader.sizeUncompressed << " bytes...    ";

            const uint8_t* dataPtr = file.ptr + fi.offset + sizeof(FSPakLocalFileHeader);
            dataPtr += localHeader.nameLength + localHeader.extraFieldLen;

            fs::path fullPath = outputPath / fi.name;
            std::error_code ec;
            fs::create_directories(fullPath.parent_path(), ec);

            if (!localHeader.compression) {
                if (!DumpToWinFile(fullPath, dataPtr, localHeader.sizeUncompressed)) {
                    std::cout << "!!! Failed to write file !!!" << std::endl;
                } else {
                    std::cout << "SUCCEEDED" << std::endl;
                }
            } else {
                std::vector<uint8_t> u_data(localHeader.sizeUncompressed);

                z_stream streamIn = {};
                streamIn.next_in = (uint8_t*)dataPtr;
                streamIn.avail_in = localHeader.sizeCompressed;
                streamIn.next_out = u_data.data();
                streamIn.avail_out = localHeader.sizeUncompressed;
                inflateInit2_(&streamIn, -15, ZLIB_VERSION, sizeof(z_stream));

                int res = inflate(&streamIn, Z_FINISH);
                inflateEnd(&streamIn);

                if (Z_STREAM_END == res) {
                    if (!DumpToWinFile(fullPath, u_data.data(), u_data.size())) {
                        std::cout << "!!! Failed to write file !!!" << std::endl;
                    } else {
                        std::cout << "SUCCEEDED" << std::endl;
                    }
                } else {
                    std::cout << "!!! Decompression FAILED !!!" << std::endl;
                }
            }
        }

        UnmapWinFile(file);
    }

    return 0;
}
