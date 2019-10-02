// Copyright 2017 Citra Emulator Project / 2019 threeSD Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cinttypes>
#include <cmath>
#include <cstring>
#include <memory>
#include <cryptopp/aes.h>
#include <cryptopp/modes.h>
#include <cryptopp/sha.h>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/common_funcs.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "core/data_container.h"
#include "core/key/key.h"
#include "core/ncch/ncch_container.h"

namespace Core {

constexpr u32 MakeMagic(char a, char b, char c, char d) {
    return a | b << 8 | c << 16 | d << 24;
}

static const int kMaxSections = 8;   ///< Maximum number of sections (files) in an ExeFs
static const int kBlockSize = 0x200; ///< Size of ExeFS blocks (in bytes)

NCCHContainer::NCCHContainer(const std::string& root_folder, const std::string& filepath)
    : root_folder(root_folder), filepath(filepath) {
    file = SDMCFile(root_folder, filepath, "rb");
}

ResultStatus NCCHContainer::OpenFile(const std::string& root_folder, const std::string& filepath) {
    this->root_folder = root_folder;
    this->filepath = filepath;
    file = SDMCFile(root_folder, filepath, "rb");

    if (!file.IsOpen()) {
        LOG_WARNING(Service_FS, "Failed to open {}", filepath);
        return ResultStatus::Error;
    }

    LOG_DEBUG(Service_FS, "Opened {}", filepath);
    return ResultStatus::Success;
}

ResultStatus NCCHContainer::Load() {
    LOG_INFO(Service_FS, "Loading NCCH from file {}", filepath);
    if (is_loaded)
        return ResultStatus::Success;

    if (file.IsOpen()) {
        // Reset read pointer in case this file has been read before.
        file.Seek(0, SEEK_SET);

        if (file.ReadBytes(&ncch_header, sizeof(NCCH_Header)) != sizeof(NCCH_Header))
            return ResultStatus::Error;

        // Verify we are loading the correct file type...
        if (MakeMagic('N', 'C', 'C', 'H') != ncch_header.magic)
            return ResultStatus::ErrorInvalidFormat;

        has_header = true;
        bool failed_to_decrypt = false;
        if (!ncch_header.no_crypto) {
            is_encrypted = true;

            // Find primary key
            if (ncch_header.fixed_key) {
                LOG_DEBUG(Service_FS, "Fixed-key crypto");
                primary_key.fill(0);
            } else {
                std::array<u8, 16> key_y_primary;

                std::copy(ncch_header.signature, ncch_header.signature + key_y_primary.size(),
                          key_y_primary.begin());

                Key::SetKeyY(Key::KeySlotID::NCCHSecure1, key_y_primary);
                if (!Key::IsNormalKeyAvailable(Key::KeySlotID::NCCHSecure1)) {
                    LOG_ERROR(Service_FS, "Secure1 KeyX missing");
                    failed_to_decrypt = true;
                }
                primary_key = Key::GetNormalKey(Key::KeySlotID::NCCHSecure1);
            }

            // Find CTR for each section
            // Written with reference to
            // https://github.com/d0k3/GodMode9/blob/99af6a73be48fa7872649aaa7456136da0df7938/arm9/source/game/ncch.c#L34-L52
            if (ncch_header.version == 0 || ncch_header.version == 2) {
                LOG_DEBUG(Loader, "NCCH version 0/2");
                // In this version, CTR for each section is a magic number prefixed by partition ID
                // (reverse order)
                std::reverse_copy(ncch_header.partition_id, ncch_header.partition_id + 8,
                                  exheader_ctr.begin());
                exefs_ctr = exheader_ctr;
                exheader_ctr[8] = 1;
                exefs_ctr[8] = 2;
            } else if (ncch_header.version == 1) {
                LOG_DEBUG(Loader, "NCCH version 1");
                // In this version, CTR for each section is the section offset prefixed by partition
                // ID, as if the entire NCCH image is encrypted using a single CTR stream.
                std::copy(ncch_header.partition_id, ncch_header.partition_id + 8,
                          exheader_ctr.begin());
                exefs_ctr = exheader_ctr;
                auto u32ToBEArray = [](u32 value) -> std::array<u8, 4> {
                    return std::array<u8, 4>{
                        static_cast<u8>(value >> 24),
                        static_cast<u8>((value >> 16) & 0xFF),
                        static_cast<u8>((value >> 8) & 0xFF),
                        static_cast<u8>(value & 0xFF),
                    };
                };
                auto offset_exheader = u32ToBEArray(0x200); // exheader offset
                auto offset_exefs = u32ToBEArray(ncch_header.exefs_offset * kBlockSize);
                std::copy(offset_exheader.begin(), offset_exheader.end(),
                          exheader_ctr.begin() + 12);
                std::copy(offset_exefs.begin(), offset_exefs.end(), exefs_ctr.begin() + 12);
            } else {
                LOG_ERROR(Service_FS, "Unknown NCCH version {}", ncch_header.version);
                failed_to_decrypt = true;
            }
        } else {
            LOG_DEBUG(Service_FS, "No crypto");
            is_encrypted = false;
        }

        // System archives and DLC don't have an extended header but have RomFS
        if (ncch_header.extended_header_size) {
            auto read_exheader = [this](SDMCFile& file) {
                const std::size_t size = sizeof(exheader_header);
                return file && file.ReadBytes(&exheader_header, size) == size;
            };

            if (!read_exheader(file)) {
                return ResultStatus::Error;
            }

            if (is_encrypted) {
                // This ID check is masked to low 32-bit as a toleration to ill-formed ROM created
                // by merging games and its updates.
                if ((exheader_header.system_info.jump_id & 0xFFFFFFFF) ==
                    (ncch_header.program_id & 0xFFFFFFFF)) {
                    LOG_WARNING(Service_FS, "NCCH is marked as encrypted but with decrypted "
                                            "exheader. Force no crypto scheme.");
                    is_encrypted = false;
                } else {
                    if (failed_to_decrypt) {
                        LOG_ERROR(Service_FS, "Failed to decrypt");
                        return ResultStatus::ErrorEncrypted;
                    }
                    CryptoPP::byte* data = reinterpret_cast<CryptoPP::byte*>(&exheader_header);
                    CryptoPP::CTR_Mode<CryptoPP::AES>::Decryption(
                        primary_key.data(), primary_key.size(), exheader_ctr.data())
                        .ProcessData(data, data, sizeof(exheader_header));
                }
            }

            u32 entry_point = exheader_header.codeset_info.text.address;
            u32 code_size = exheader_header.codeset_info.text.code_size;
            u32 stack_size = exheader_header.codeset_info.stack_size;
            u32 bss_size = exheader_header.codeset_info.bss_size;
            u32 core_version = exheader_header.arm11_system_local_caps.core_version;
            u8 priority = exheader_header.arm11_system_local_caps.priority;
            u8 resource_limit_category =
                exheader_header.arm11_system_local_caps.resource_limit_category;

            LOG_DEBUG(Service_FS, "Name:                        {}",
                      exheader_header.codeset_info.name);
            LOG_DEBUG(Service_FS, "Program ID:                  {:016X}", ncch_header.program_id);
            LOG_DEBUG(Service_FS, "Entry point:                 0x{:08X}", entry_point);
            LOG_DEBUG(Service_FS, "Code size:                   0x{:08X}", code_size);
            LOG_DEBUG(Service_FS, "Stack size:                  0x{:08X}", stack_size);
            LOG_DEBUG(Service_FS, "Bss size:                    0x{:08X}", bss_size);
            LOG_DEBUG(Service_FS, "Core version:                {}", core_version);
            LOG_DEBUG(Service_FS, "Thread priority:             0x{:X}", priority);
            LOG_DEBUG(Service_FS, "Resource limit category:     {}", resource_limit_category);
            LOG_DEBUG(Service_FS, "System Mode:                 {}",
                      static_cast<int>(exheader_header.arm11_system_local_caps.system_mode));

            has_exheader = true;
        }

        // DLC can have an ExeFS and a RomFS but no extended header
        if (ncch_header.exefs_size) {
            exefs_offset = ncch_header.exefs_offset * kBlockSize;
            u32 exefs_size = ncch_header.exefs_size * kBlockSize;

            LOG_DEBUG(Service_FS, "ExeFS offset:                0x{:08X}", exefs_offset);
            LOG_DEBUG(Service_FS, "ExeFS size:                  0x{:08X}", exefs_size);
            file.Seek(exefs_offset, SEEK_SET);
            if (file.ReadBytes(&exefs_header, sizeof(ExeFs_Header)) != sizeof(ExeFs_Header))
                return ResultStatus::Error;

            if (is_encrypted) {
                CryptoPP::byte* data = reinterpret_cast<CryptoPP::byte*>(&exefs_header);
                CryptoPP::CTR_Mode<CryptoPP::AES>::Decryption(primary_key.data(),
                                                              primary_key.size(), exefs_ctr.data())
                    .ProcessData(data, data, sizeof(exefs_header));
            }

            exefs_file = SDMCFile(root_folder, filepath, "rb");
            has_exefs = true;
        }
    }

    is_loaded = true;
    return ResultStatus::Success;
}

ResultStatus NCCHContainer::LoadSectionExeFS(const char* name, std::vector<u8>& buffer) {
    ResultStatus result = Load();
    if (result != ResultStatus::Success)
        return result;

    if (!exefs_file.IsOpen())
        return ResultStatus::Error;

    LOG_DEBUG(Service_FS, "{} sections:", kMaxSections);
    // Iterate through the ExeFs archive until we find a section with the specified name...
    for (unsigned section_number = 0; section_number < kMaxSections; section_number++) {
        const auto& section = exefs_header.section[section_number];

        if (strcmp(section.name, name) == 0) {
            LOG_DEBUG(Service_FS, "{} - offset: 0x{:08X}, size: 0x{:08X}, name: {}", section_number,
                      section.offset, section.size, section.name);

            s64 section_offset = (section.offset + exefs_offset + sizeof(ExeFs_Header));
            exefs_file.Seek(section_offset, SEEK_SET);

            CryptoPP::CTR_Mode<CryptoPP::AES>::Decryption dec(primary_key.data(),
                                                              primary_key.size(), exefs_ctr.data());
            dec.Seek(section.offset + sizeof(ExeFs_Header));

            buffer.resize(section.size);
            if (exefs_file.ReadBytes(&buffer[0], section.size) != section.size)
                return ResultStatus::Error;
            if (is_encrypted) {
                dec.ProcessData(&buffer[0], &buffer[0], section.size);
            }

            return ResultStatus::Success;
        }
    }
    return ResultStatus::ErrorNotUsed;
}

ResultStatus NCCHContainer::ReadProgramId(u64_le& program_id) {
    ResultStatus result = Load();
    if (result != ResultStatus::Success)
        return result;

    if (!has_header)
        return ResultStatus::ErrorNotUsed;

    program_id = ncch_header.program_id;
    return ResultStatus::Success;
}

ResultStatus NCCHContainer::ReadExtdataId(u64& extdata_id) {
    ResultStatus result = Load();
    if (result != ResultStatus::Success)
        return result;

    if (!has_exheader)
        return ResultStatus::ErrorNotUsed;

    if (exheader_header.arm11_system_local_caps.storage_info.other_attributes >> 1) {
        // Using extended save data access
        // There would be multiple possible extdata IDs in this case. The best we can do for now is
        // guessing that the first one would be the main save.
        const std::array<u64, 6> extdata_ids{{
            exheader_header.arm11_system_local_caps.storage_info.extdata_id0.Value(),
            exheader_header.arm11_system_local_caps.storage_info.extdata_id1.Value(),
            exheader_header.arm11_system_local_caps.storage_info.extdata_id2.Value(),
            exheader_header.arm11_system_local_caps.storage_info.extdata_id3.Value(),
            exheader_header.arm11_system_local_caps.storage_info.extdata_id4.Value(),
            exheader_header.arm11_system_local_caps.storage_info.extdata_id5.Value(),
        }};
        for (u64 id : extdata_ids) {
            if (id) {
                // Found a non-zero ID, use it
                extdata_id = id;
                return ResultStatus::Success;
            }
        }

        return ResultStatus::ErrorNotUsed;
    }

    extdata_id = exheader_header.arm11_system_local_caps.storage_info.ext_save_data_id;
    return ResultStatus::Success;
}

bool NCCHContainer::HasExeFS() {
    ResultStatus result = Load();
    if (result != ResultStatus::Success)
        return false;

    return has_exefs;
}

bool NCCHContainer::HasExHeader() {
    ResultStatus result = Load();
    if (result != ResultStatus::Success)
        return false;

    return has_exheader;
}

#pragma pack(push, 1)
struct RomFSIVFCHeader {
    u32_le magic;
    u32_le version;
    u32_le master_hash_size;
    std::array<LevelDescriptor, 3> levels;
    INSERT_PADDING_BYTES(0xC);
};
static_assert(sizeof(RomFSIVFCHeader) == 0x60, "Size of RomFSIVFCHeader is incorrect");
#pragma pack(pop)

std::vector<u8> LoadSharedRomFS(const std::vector<u8>& data) {
    NCCH_Header header;
    ASSERT_MSG(data.size() >= sizeof(header), "NCCH size is too small");
    std::memcpy(&header, data.data(), sizeof(header));

    const std::size_t offset = header.romfs_offset * 0x200; // 0x200: Media unit
    RomFSIVFCHeader ivfc;
    ASSERT_MSG(data.size() >= offset + sizeof(ivfc), "NCCH size is too small");
    std::memcpy(&ivfc, data.data() + offset, sizeof(ivfc));

    ASSERT_MSG(ivfc.magic == MakeMagic('I', 'V', 'F', 'C'), "IVFC magic is incorrect");
    ASSERT_MSG(ivfc.version == 0x10000, "IVFC version is incorrect");

    std::vector<u8> result(ivfc.levels[2].size);

    // Calculation from ctrtool
    const std::size_t data_offset =
        offset + Common::AlignUp(sizeof(ivfc) + ivfc.master_hash_size,
                                 std::pow(2, ivfc.levels[2].block_size));
    ASSERT_MSG(data.size() >= data_offset + ivfc.levels[2].size);
    std::memcpy(result.data(), data.data() + data_offset, ivfc.levels[2].size);

    return result;
}

} // namespace Core