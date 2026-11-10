#ifndef MYVAVA_ARCHIVE_CIPHER_H
#define MYVAVA_ARCHIVE_CIPHER_H
#include "files.h"
#include "sealed_file.h"
uint64_t archive_cipher_size(uint64_t plaintext);
bool archive_cipher_offset(int fd,uint32_t total,uint32_t *offset,bool repair);
bool archive_cipher_write(int fd,const struct seal_keys *,const struct myvava_file *,uint32_t offset,const uint8_t *,size_t);
int archive_cipher_read(int fd,const struct seal_keys *,const struct myvava_file *,uint32_t offset,void *,size_t);
#endif
