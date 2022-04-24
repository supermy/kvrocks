#pragma once
#include <string>

// CRC即循环冗余校验码（Cyclic Redundancy Check）：是数据通信领域中最常用的一种查错校验码，其特征是信息字段和校验字段的长度可以任意选定。奇偶校验虽然简单，但是漏检率太高，而CRC则要低的多，所以大多数都是使用CRC来校验。CRC也称为多项式码。
// 循环冗余检查（CRC）是一种数据传输检错功能，对数据进行多项式计算，并将得到的结果附在帧的后面，接收设备也执行类似的算法，进而可以保证在软件层次上数据传输的正确性和完整性。
// crc16
#define HASH_SLOTS_MASK 0x3fff
#define HASH_SLOTS_SIZE (HASH_SLOTS_MASK + 1)      // 16384
#define HASH_SLOTS_MAX_ITERATIONS 50

uint16_t crc16(const char *buf, int len);
uint16_t GetSlotNumFromKey(const std::string &key);
std::string GetTagFromKey(const std::string &key);
