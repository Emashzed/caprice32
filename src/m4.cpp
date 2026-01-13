/* Caprice32 - Amstrad CPC Emulator
   (c) Copyright 1997-2004 Ulrich Doewich

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
   This file includes video filters from the SMS Plus/SDL 
   sega master system emulator :
   (c) Copyright Gregory Montoir
   http://membres.lycos.fr/cyxdown/smssdl/
*/

/*
   This file includes video filters from MAME
   (Multiple Arcade Machine Emulator) :
   (c) Copyright The MAME Team
   http://www.mame.net/
*/

#include <algorithm>
#include <iostream>
#include <string>
#include <filesystem>
#include <unordered_set>
namespace fs = std::filesystem;

#include "m4.h"
#include "cap32.h"
#include "log.h"

#ifdef DEBUG
#define M4_DEBUG
#endif

extern t_CPC CPC;
extern byte *memmap_ROM[256];

static inline unsigned to_u8(byte b) {
    return static_cast<unsigned>(b);
}

#ifdef M4_DEBUG
#define LOG_M4(x) LOG_INFO(x)

// Returns a formatted hex dump as a string.
// - data: pointer to bytes
// - len: number of bytes
// - base_addr: optional displayed "address" base (0 by default). Pass a real address if you want.
std::string to_hex_dump(const byte* data,
                     std::size_t len,
                     std::uintptr_t base_addr = 0,
                     std::size_t bytes_per_line = 16)
{
    if (!data || len == 0 || bytes_per_line == 0) return "<Empty>";

    // Width for the address column (at least 8 hex digits, often 16 on 64-bit)
    const int addr_width = (sizeof(std::uintptr_t) >= 8) ? 16 : 8;

    std::ostringstream out;
    out << std::hex << std::setfill('0');

    for (std::size_t i = 0; i < len; i += bytes_per_line) {
        const std::size_t line_len = std::min(bytes_per_line, len - i);

        // Address
        out << std::setw(addr_width) << (base_addr + i) << "  ";

        // Hex bytes
        for (std::size_t j = 0; j < bytes_per_line; ++j) {
            if (j < line_len) {
                out << std::setw(2) << to_u8(data[i + j]);
            } else {
                out << "  ";
            }
            if (j + 1 != bytes_per_line) out << ' ';
            if (j == 7) out << ' '; // extra gap in the middle (like many hexdumps)
        }

        out << "  ";

        // ASCII / printable string
        for (std::size_t j = 0; j < line_len; ++j) {
            unsigned v = to_u8(data[i + j]);
            char c = static_cast<char>(v);
            out << (c>=32 ? c : '.');
        }

        out << '\n';
    }

    return out.str();
}

#else
#define LOG_M4(x)
#endif

#define M4_C_OPEN			   0x4301
#define M4_C_READ		   	0x4302
#define M4_C_WRITE			0x4303
#define M4_C_CLOSE			0x4304
#define M4_C_SEEK		   	0x4305
#define M4_C_READDIR			0x4306
#define M4_C_EOF		   	0x4307
#define M4_C_CD				0x4308
#define M4_C_FREE  			0x4309
#define M4_C_FTELL			0x430A
#define M4_C_READSECTOR		0x430B
#define M4_C_WRITESECTOR	0x430C
#define M4_C_FORMATTRACK	0x430D
#define M4_C_ERASEFILE		0x430E
#define M4_C_RENAME			0x430F
#define M4_C_MAKEDIR			0x4310
#define M4_C_FSIZE			0x4311
#define M4_C_READ2			0x4312
#define M4_C_GETPATH			0x4313
#define M4_C_SDREAD			0x4314
#define M4_C_SDWRITE			0x4315
#define M4_C_FSTAT			0x4316
#define M4_C_NMI			   0x431D
#define M4_C_HTTPGET			0x4320
#define M4_C_SETNETWORK		0x4321
#define M4_C_M4OFF			0x4322
#define M4_C_NETSTAT			0x4323
#define M4_C_TIME		   	0x4324
#define M4_C_DIRSETARGS		0x4325
#define M4_C_VERSION			0x4326
#define M4_C_UPGRADE			0x4327
#define M4_C_HTTPGETMEM		0x4328
#define M4_C_COPYBUF			0x4329
#define M4_C_COPYFILE		0x432A
#define M4_C_ROMSUPDATE		0x432B
#define M4_C_CMDRBTRUN     0x432D
#define M4_C_NETSOCKET		0x4331
#define M4_C_NETCONNECT		0x4332
#define M4_C_NETCLOSE		0x4333
#define M4_C_NETSEND			0x4334
#define M4_C_NETRECV      	0x4335
#define M4_C_NETHOSTIP    	0x4336
#define M4_C_NETRSSI       0x4337
#define M4_C_NETBIND			0x4338
#define M4_C_NETLISTEN		0x4339
#define M4_C_NETACCEPT		0x433A
#define M4_C_GETNETWORK		0x433B
#define M4_C_WIFIPOW       0x433C
#define M4_C_ROMCP         0x43FC
#define M4_C_ROMWRITE      0x43FD
#define M4_C_CONFIG        0x43FE
#define M4_C_ROMLOW        0x433D

#define M4_FILEACCESS_READ 				1
#define M4_FILEACCESS_WRITE				2
#define M4_FILEACCESS_CREATE_NEW			4
#define M4_FILEACCESS_CREATE_ALWAYS		8
#define M4_FILEACCESS_OPEN_ALWAYS		16
#define M4_FILEACCESS_REALMODE			128

#define M4_ROM_SLOT 6
#define M4_ROM_RESPONSE_OFFEST 0xE800
#define M4_ROM_WORKSPACE_OFFSET (M4_ROM_RESPONSE_OFFEST + 0x0A00)
#define M4_ROM_CONFIG_OFFSET (M4_ROM_RESPONSE_OFFEST + 0x0C00)

#define M4_COMMAND_BUFFER_SIZE 1024
#define M4_MAX_FILE_DESCRIPTORS 10
#define M4_COMMAND_SIZE 0x800

byte *m4_command;
int m4_command_idx = 0;
std::string m4_current_dir;
FILE** m4_file_descriptors; // Used for "REAL MODE" file access
bool m4_amsdos_realmode;
int m4_current_amsdos_descriptor;
fs::directory_iterator m4_directory_iterator;
fs::directory_iterator m4_directory_end;
bool m4_directory_initialized = false;

struct FileInfo {
    std::string long_name;
    std::string display_name;
    std::uintmax_t size;
    std::string display_size;
    bool is_directory;
};

static std::string rpad(std::string s, std::size_t width) {
    if (s.size() < width) s.append(width - s.size(), ' ');
    else if (s.size() > width) s.resize(width);
    return s;
}

static std::string lpad(std::string s, std::size_t width) {
    if (s.size() < width) s.insert(0, width - s.size(), ' ');
    else if (s.size() > width) s.resize(width);
    return s;
}

static std::string trim(std::string s) {
    // Remove control characters (< 32)
    s.erase(std::remove_if(s.begin(), s.end(),
        [](unsigned char c) { return c < 32; }),
        s.end());

    // Trim leading spaces
    auto first = std::find_if(s.begin(), s.end(),
        [](unsigned char c) { return c != ' '; });

    // Trim trailing spaces
    auto last = std::find_if(s.rbegin(), s.rend(),
        [](unsigned char c) { return c != ' '; }).base();

    if (first < last)
        s = std::string(first, last);
    else
        s.clear();

    return s;
}

static std::string truncate(const std::string& s, std::size_t n) {
    return (s.size() <= n) ? s : s.substr(0, n);
}

std::string get_display_size(const FileInfo& file) {
   if (file.is_directory) {
      return "";
   } else {
      int size_in_kb = static_cast<int>((file.size + 1023) / 1024);
      if (size_in_kb == 0) size_in_kb = 1;
      return lpad(std::to_string(size_in_kb), 4) + "K";
   }
}

std::string get_display_name(const FileInfo& file, int max_name_len) {
   if (max_name_len > 80) {
      max_name_len = 80;
   }

   // LFN mode
   if (max_name_len != -1) {
      std::string name = file.long_name;
      if (file.is_directory) {
         name = ">" + name;
      }
      if (name.size() <= static_cast<size_t>(max_name_len)) {
         return name;
      } else {
         return truncate(name, static_cast<std::size_t>(max_name_len - 1)) + "~";
      }
   }

   // 8.3 mode
   if (file.is_directory) {
      std::string name = ">" + file.long_name;
      int directory_max_len = 17;
      if (name.size() <= static_cast<size_t>(directory_max_len)) {
         return rpad(name, directory_max_len);
      } else {
         return truncate(name, static_cast<std::size_t>(directory_max_len - 1)) + "~";
      }
   } else {
      std::string file_base = fs::path(file.long_name).stem().string();
      file_base = trim(file_base);
      std::string extension = fs::path(file.long_name).extension().string();
      extension = trim(extension);

      if (file_base.size() <= 8) {
         file_base = rpad(file_base, 8);
      } else {
         file_base = truncate(file_base, 7) + "~";
      }
      return file_base + rpad(extension, 4);
   }
}

fs::path get_current_path() {
    return fs::path(CPC.m4_path + m4_current_dir);
}

void force_dir_data_refresh() {
    m4_directory_initialized = false;
}

// -- M4 command handling --------------------------------------------------------------------------------------------

byte* get_m4_rom() {
   return memmap_ROM[M4_ROM_SLOT];
}

byte* get_m4_response() {
   return get_m4_rom() + (M4_ROM_RESPONSE_OFFEST & 0x3FFF);
}

void m4_answer_bytes(int command, const byte* data, int size) {
   memset(get_m4_response(), 0, M4_COMMAND_SIZE); // clear previous response
   byte* response = get_m4_response();

   int response_size = size + 2;
   response[0] = static_cast<byte>(response_size); // total response size
   response[1] = static_cast<byte>(command & 0xFF);
   response[2] = static_cast<byte>((command >> 8) & 0xFF);
   if (size > 0 && data != nullptr) {
      memcpy(response + 3, data, static_cast<size_t>(size));
   }

   LOG_M4("M4 answering command 0x" << std::hex << command << std::dec << " with size " << response_size << "\n"
            << to_hex_dump(response, size + 3));
}

void m4_answer_void(int command) {
   m4_answer_bytes(command, nullptr, 0);
}

void m4_answer_string(int command, const char* data) {
   m4_answer_bytes(command, reinterpret_cast<const byte*>(data), static_cast<int>(strlen(data) + 1));
}

void m4_answer_code(int command, byte code) {
   m4_answer_bytes(command, &code, 1);
}

void m4_answer_codes(int command, byte code1, byte code2) {
   byte codes[2] = { code1, code2 };
   m4_answer_bytes(command, codes, 2);
}

void m4_answer_uint32(int command, uint32_t value) {
   byte data[4];
   data[0] = static_cast<byte>(value & 0xFF);
   data[1] = static_cast<byte>((value >> 8) & 0xFF);
   data[2] = static_cast<byte>((value >> 16) & 0xFF);
   data[3] = static_cast<byte>((value >> 24) & 0xFF);
   m4_answer_bytes(command, data, 4);
}


// C_OPEN			0x4301		data[0] = mode, data[1] = filename. Return data[0] = fd (*), data[1] = res.
void m4_command_open(byte* data, int size) {
   LOG_M4("M4 C_OPEN received with size " << size << "\n" 
            << to_hex_dump(data, size));  

   int mode = data[0];
   std::string filename(reinterpret_cast<char*>(data + 1), static_cast<size_t>(size - 1));

   //split into path, file base, and extension
   std::string path = fs::path(filename).parent_path().string();
   path = trim(path);
   std::string file_base = fs::path(filename).stem().string();
   file_base = trim(file_base);
   std::string extension = fs::path(filename).extension().string();
   extension = trim(extension);

   LOG_INFO("  parsed path='" << path << "', file_base='" << file_base << "', extension='" << extension << "'");

   fs::path file_path;
   if (!path.empty() && path[0] == '/') {
      file_path = fs::path(CPC.m4_path) / (path.substr(1) + file_base + extension);
   } else {
      file_path = get_current_path() / (path + file_base + extension);
   }

   LOG_INFO("  opening file '" << file_path.string() << "' with mode " << mode);

   fs::path file_to_open = file_path;

   auto is_valid_file = [](const fs::path& p) {
      return fs::exists(p) && fs::is_regular_file(p);
   };

   if (mode & M4_FILEACCESS_READ) {
      LOG_INFO("  read access requested");
      if (!is_valid_file(file_path) && extension.empty()) {
         LOG_INFO("  file not found, trying .BAS and .BIN extensions");
         if (is_valid_file(file_path.string() + ".BAS")) {
            LOG_INFO("  found .BAS file");
            file_to_open = file_path.string() + ".BAS";
         } else if (is_valid_file(file_path.string() + ".BIN")) {
            LOG_INFO("  found .BIN file");
            file_to_open = file_path.string() + ".BIN";
         } else {
            LOG_INFO("  no valid file found");
            m4_answer_codes(M4_C_OPEN, 0xff, 0xff); // file not found
            return;
         }
      }
   }

   int fd = -1;
   if (mode & M4_FILEACCESS_REALMODE) {
      for (int i = 0; i < M4_MAX_FILE_DESCRIPTORS; i++) {
         if (m4_file_descriptors[i] == nullptr) {
            fd = i;
            break;
         }
      }
   } else {
      if (m4_file_descriptors[1] != nullptr) {
         fclose(m4_file_descriptors[1]);
         m4_file_descriptors[1] = nullptr;
      }
      fd = 1; // single generic AMSDOS file
   }

   if (fd == -1) {
      LOG_M4("  no free file descriptor available");
      m4_answer_codes(M4_C_OPEN, 0xff, 0xff);
      return;
   }

   const char* fopen_mode = (mode & M4_FILEACCESS_READ) ? "rb" : ((mode & M4_FILEACCESS_WRITE) ? "wb" : "rb+");
   FILE* f = fopen(file_to_open.string().c_str(), fopen_mode);
   if (f != nullptr) {
      m4_file_descriptors[fd] = f;
      m4_current_amsdos_descriptor = fd;
      m4_amsdos_realmode = (mode & M4_FILEACCESS_REALMODE) != 0;
      LOG_M4("  opened file, assigned fd " << fd);
      m4_answer_codes(M4_C_OPEN, fd, 0x00);
   } else {
      LOG_M4("  failed to open file");
      m4_answer_codes(M4_C_OPEN, 0xff, 0xff);
   }
}

// C_FSIZE			0x4311		Implemented v1.0.5. data[0] = fd. Return data[0-3] = file size
void m4_command_fsize(byte* data, int size) {
   LOG_M4("M4 C_FSIZE received with size " << size << "\n" 
            << to_hex_dump(data, size));  
   int fd = m4_amsdos_realmode ? data[0] : m4_current_amsdos_descriptor;
   if (fd < 0 || fd >= M4_MAX_FILE_DESCRIPTORS || m4_file_descriptors[fd] == nullptr) {
      LOG_M4("  invalid file descriptor " << fd);
      m4_answer_code(M4_C_FSIZE, 0xff);
      return;
   }
   FILE* f = m4_file_descriptors[fd];
   long current_pos = ftell(f);
   fseek(f, 0, SEEK_END);
   long file_size = ftell(f);
   fseek(f, current_pos, SEEK_SET);
   m4_answer_uint32(M4_C_FSIZE, static_cast<uint32_t>(file_size));
}

// C_CLOSE			0x4304		data[0] = fd. Return data[0] = res.
void m4_command_close(byte* data, int size) {
   LOG_M4("M4 C_CLOSE received with size " << size << "\n" 
            << to_hex_dump(data, size));  
   int fd = m4_amsdos_realmode ? data[0] : m4_current_amsdos_descriptor;
   if (fd < 0 || fd >= M4_MAX_FILE_DESCRIPTORS || m4_file_descriptors[fd] == nullptr) {
      LOG_M4("  invalid file descriptor " << fd);
      m4_answer_code(M4_C_CLOSE, 0xff);
      return;
   }
   fclose(m4_file_descriptors[fd]);
   m4_file_descriptors[fd] = nullptr;
   m4_amsdos_realmode = false;
   m4_current_amsdos_descriptor = -1;
   m4_answer_code(M4_C_CLOSE, 0x00);
}

// C_READ			0x4302		data[0] = fd, data[1..2] = read size. Return data[0] = res, data[1..] = data.
void m4_command_read(byte* data, int size) {
   LOG_M4("M4 C_READ received with size " << size << "\n" 
            << to_hex_dump(data, size));  
   int fd = m4_amsdos_realmode ? data[0] : m4_current_amsdos_descriptor;
   int read_size = data[1] | (data[2] << 8);
   if (fd < 0 || fd >= M4_MAX_FILE_DESCRIPTORS || m4_file_descriptors[fd] == nullptr) {
      LOG_M4("  invalid file descriptor " << fd);
      m4_answer_code(M4_C_READ, 0xff);
      return;
   }
   FILE* f = m4_file_descriptors[fd];
   byte* buffer = new byte[read_size + 1];
   fread(buffer + 1, 1, static_cast<size_t>(read_size), f);
   buffer[0] = 0x00; // success

   m4_answer_bytes(M4_C_READ, buffer, read_size + 1);
   delete[] buffer;
}

// C_WRITE			0x4303		data[0] = fd, data[1..] = data. Return data[0] = res.
void m4_command_write(byte* data, int size) {
   LOG_M4("M4 C_WRITE received with size " << size << "\n" 
            << to_hex_dump(data, size));  
   int fd = m4_amsdos_realmode ? data[0] : m4_current_amsdos_descriptor;
   if (fd < 0 || fd >= M4_MAX_FILE_DESCRIPTORS || m4_file_descriptors[fd] == nullptr) {
      LOG_M4("  invalid file descriptor " << fd);
      m4_answer_code(M4_C_WRITE, 0xff);
      return;
   }
   FILE* f = m4_file_descriptors[fd];
   fwrite(data + 1, 1, static_cast<size_t>(size - 1), f);
   m4_answer_code(M4_C_WRITE, 0x00);
}

// C_FTELL			0x430A		Implemented v1.0.5. data[0] = fd. Return data[0-3] = current file pos
void m4_command_ftell(byte* data, int size) {
   LOG_M4("M4 C_FTELL received with size " << size << "\n" 
            << to_hex_dump(data, size));  
   int fd = m4_amsdos_realmode ? data[0] : m4_current_amsdos_descriptor;
   if (fd < 0 || fd >= M4_MAX_FILE_DESCRIPTORS || m4_file_descriptors[fd] == nullptr) {
      LOG_M4("  invalid file descriptor " << fd);
      m4_answer_code(M4_C_FTELL, 0xff);
      return;
   }
   FILE* f = m4_file_descriptors[fd];
   long current_pos = ftell(f);
   m4_answer_uint32(M4_C_FTELL, static_cast<uint32_t>(current_pos));
}

// C_SEEK			0x4305		data[0] = fd, data[1..5] = offset. Return data[0] = res.
void m4_command_seek(byte* data, int size) {
   LOG_M4("M4 C_SEEK received with size " << size << "\n" 
            << to_hex_dump(data, size));  
   int fd = m4_amsdos_realmode ? data[0] : m4_current_amsdos_descriptor;
   uint32_t offset = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
   if (fd < 0 || fd >= M4_MAX_FILE_DESCRIPTORS || m4_file_descriptors[fd] == nullptr) {
      LOG_M4("  invalid file descriptor " << fd);
      m4_answer_code(M4_C_SEEK, 0xff);
      return;
   }
   FILE* f = m4_file_descriptors[fd];
   fseek(f, static_cast<long>(offset), SEEK_SET);
   m4_answer_code(M4_C_SEEK, 0x00);
}

// C_CMDRBTRUN	   0x432D		(UNDOCUMENTED) data = file to run after reboot
void m4_command_rbtrun(byte* data, int size) {
   LOG_M4("M4 C_CMDRBTRUN received with size " << size << "\n" 
            << to_hex_dump(data, size));  
   // TODO
}

// C_READDIR			0x4306		No args. Return data[0] = dir entry (formatted). Loop until response size == 2.
//							From v110 b9, one argument can be given data[0] = max name len (to support lfn).
void m4_command_readdir(byte* data, int size) {
   LOG_M4("M4 C_READDIR received with size " << size << "\n" 
            << to_hex_dump(data, size));  

   std::error_code ec;

   if (!m4_directory_initialized) {
        ec.clear();
        m4_directory_iterator = fs::directory_iterator(get_current_path(), ec);
        m4_directory_initialized = true;

        if (ec) {
            LOG_ERROR("Iterator init error: " << ec.message());
            return;
        }
   }

   if (m4_directory_end == m4_directory_iterator) {
      m4_answer_void(M4_C_READDIR); // end of directory
      m4_directory_initialized = false; // reset for next time
      return;
   }

   const fs::directory_entry& entry = *m4_directory_iterator++;
   LOG_INFO("  found entry: " << entry.path().filename().string());

   int max_name_len = (size <= 0 ? -1 : data[0]);

   FileInfo info;
   info.long_name = entry.path().filename().string();
   info.is_directory = entry.is_directory(ec);
   info.size = info.is_directory ? 0 : entry.file_size(ec);
   info.display_name = get_display_name(info, max_name_len);
   info.display_size = get_display_size(info);

   int separator_size = (max_name_len != -1) ? 1 : 0;
   int payload_size = static_cast<int>(info.display_name.size() + separator_size + info.display_size.size() + 1);
   byte* payload = new byte[payload_size];
   std::memcpy(payload, info.display_name.c_str(), info.display_name.size() + separator_size);
   std::memcpy(payload + info.display_name.size() + separator_size, info.display_size.c_str(), info.display_size.size() + 1);
   m4_answer_bytes(M4_C_READDIR, payload, payload_size);
   delete[] payload;
}

// C_FREE  			0x4309		Freespace sd card / DSK image.  Return data[0] = "\r\n%iK free\r\n\r\n".
void m4_command_free(byte* data, int size) {
   LOG_M4("M4 C_FREE received with size " << size << "\n" 
            << to_hex_dump(data, size));

   std::error_code ec;
   fs::space_info info = fs::space(get_current_path(), ec);

   if (ec) {
       LOG_ERROR("Error getting space info: " << ec.message());
       return;
   }

   std::string free_space = "\r\n" + std::to_string(info.available / 1024) + "K free\r\n\r\n";
   m4_answer_string(M4_C_FREE, free_space.c_str());
}

// C_DIRSETARGS		0x4325		data[0] = "folder/match string" (wildcards etc.) Should be used before C_READDIR, if no args, use null string. 
void m4_command_dirsetargs(byte* data, int size) {
   LOG_M4("M4 C_DIRSETARGS received with size " << size << "\n" 
            << to_hex_dump(data, size));  
   // TODO
}

// C_GETPATH			0x4313		Implemented v1.0.8. Return data = current path in ascii.
void m4_command_getpath(byte* data, int size) {
   LOG_M4("M4 C_GETPATH received with size " << size << "\n" 
            << to_hex_dump(data, size));
   force_dir_data_refresh();
   m4_answer_string(M4_C_GETPATH, m4_current_dir.c_str());
}

// C_CONFIG            0x43FE		Write to M4 ROM config area. data[0] = offset in config area (0xFF04+offset), using command size(-3).
void m4_command_config(byte* data, int size) {
   LOG_M4("M4 C_CONFIG received with size " << size << "\n" 
            << to_hex_dump(data, size));
   int offset = M4_ROM_CONFIG_OFFSET + data[0];
   LOG_M4("  setting M4 config offset 0x" << std::hex << offset);
   std::memcpy(get_m4_rom() + (offset & 0x3FFF), data + 1, size - 1);
}

// C_CD				0x4308		data[0...] = "directory name". Return data[0] = res.
void m4_command_cd(byte* data, int size) {
   LOG_M4("M4 C_CD received with size " << size << "\n" 
            << to_hex_dump(data, size));
   std::string dir(reinterpret_cast<char*>(data), static_cast<size_t>(size - 1));
   dir.erase(dir.find_last_not_of(" \n\r\t") + 1);
   if (dir == "..") {
      fs::path current_path = get_current_path();
      fs::path parent_path = current_path.parent_path();
      m4_current_dir = parent_path.string().substr(CPC.m4_path.size());
      if (m4_current_dir.empty()) m4_current_dir = "/";
   } else {
      if (dir.front() == '/') {
         m4_current_dir = dir;
      } else {
         fs::path current_path = get_current_path();
         fs::path new_path = current_path / dir;
         if (fs::exists(new_path) && fs::is_directory(new_path)) {
            if (m4_current_dir == "/") {
               m4_current_dir = m4_current_dir + dir ;
            } else {
               m4_current_dir = m4_current_dir + "/" + dir ;
            }
         } else {
            m4_answer_code(M4_C_CD, 0xff);
            return;
         }
      }
   }
   force_dir_data_refresh();
   LOG_M4("  changed directory to '" << m4_current_dir << "'");
   m4_answer_code(M4_C_CD, 0x00);
}

// C_ROMWRITE          0x43FD         Implemented ?. Write to ROM (M4 or HACK/NMI rom). data[0-1] = dest offset, data[2-3] = size, data[4] = rom (255 = NMI/HACK ROM, 0 = M4 ROM).
void m4_command_romwrite(byte* data, int size) {
   LOG_M4("M4 C_ROMWRITE received with size " << size << "\n" 
            << to_hex_dump(data, size));
   int m4_romwrite_dest_offset = data[0] | (data[1] << 8);
   int m4_romwrite_size = data[2] | (data[3] << 8);
   int m4_romwrite_rom = data[4];  // 255 = NMI/Hack ROM, 0 = M4 ROM
   LOG_M4("  dest_offset=0x" << std::hex << m4_romwrite_dest_offset 
      << ", size=0x" << m4_romwrite_size 
      << ", rom=" << std::dec << static_cast<int>(m4_romwrite_rom));
   //TODO: perform the ROM write
}

void m4_init()
{
   if (!CPC.m4) return;
   m4_command_idx = 0;
   m4_current_dir = "/";
   m4_amsdos_realmode = false;
   m4_current_amsdos_descriptor = -1;   
   force_dir_data_refresh();
   m4_command = new byte[M4_COMMAND_BUFFER_SIZE];
   m4_file_descriptors = new FILE*[M4_MAX_FILE_DESCRIPTORS];
   for (int i = 0; i < M4_MAX_FILE_DESCRIPTORS; i++) {
      m4_file_descriptors[i] = nullptr;
   }
}

void m4_close()
{
   if (!CPC.m4) return;
   delete[] m4_command;
   for (int i = 0; i < M4_MAX_FILE_DESCRIPTORS; i++) {
      if (m4_file_descriptors[i] != nullptr) {
         fclose(m4_file_descriptors[i]);
         m4_file_descriptors[i] = nullptr;}
   }
   delete[] m4_file_descriptors;
}

void m4_run_command() {
   if (!CPC.m4) return;

   int cmd_size = m4_command[0];
   int cmd = m4_command[1] | (m4_command[2] << 8);
   byte* cmd_data = m4_command + 3;
   int cmd_param_size = m4_command_idx - 3;

   switch (cmd) {
      case M4_C_OPEN:
         m4_command_open(cmd_data, cmd_param_size);
         break;
      case M4_C_CLOSE:
         m4_command_close(cmd_data, cmd_param_size);
         break;
      case M4_C_FSIZE:
         m4_command_fsize(cmd_data, cmd_param_size);
         break;
      case M4_C_READ:
         m4_command_read(cmd_data, cmd_param_size);
         break;
      case M4_C_WRITE:
         m4_command_write(cmd_data, cmd_param_size);
         break;
      case M4_C_FTELL:
         m4_command_ftell(cmd_data, cmd_param_size);
         break;
      case M4_C_SEEK:
         m4_command_seek(cmd_data, cmd_param_size);
         break;
      case M4_C_READDIR:
         m4_command_readdir(cmd_data, cmd_param_size);
         break;
      case M4_C_FREE:
         m4_command_free(cmd_data, cmd_param_size);
         break;
      case M4_C_CD:
         m4_command_cd(cmd_data, cmd_param_size);
         break;
      case M4_C_CMDRBTRUN:
         m4_command_rbtrun(cmd_data, cmd_param_size);
         break;
      case M4_C_DIRSETARGS:
         m4_command_dirsetargs(cmd_data, cmd_param_size);
         break;
      case M4_C_GETPATH:
         m4_command_getpath(cmd_data, cmd_param_size);
         break;
      case M4_C_ROMWRITE:
         m4_command_romwrite(cmd_data, cmd_param_size);
         break;
      case M4_C_CONFIG:
         m4_command_config(cmd_data, cmd_param_size);
         break;
      default:
         LOG_WARNING("**** UNHANDLED M4 COMMAND 0x" << std::hex << cmd << std::dec << " received with size " << cmd_size << "\n"
            << to_hex_dump(cmd_data, cmd_param_size));
         break;
   }

   // reset command buffer
   m4_command_idx = 0;
}

void m4_write_command(byte val) {
   if (!CPC.m4) return;

   if (m4_command_idx < M4_COMMAND_BUFFER_SIZE) {
      m4_command[m4_command_idx++] = val;
   } else {
      LOG_ERROR("M4 command buffer overflow");
   }
}
