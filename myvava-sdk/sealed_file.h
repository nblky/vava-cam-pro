#ifndef MYVAVA_SEALED_FILE_H
#define MYVAVA_SEALED_FILE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SEAL_BLOCK 4096
#define SEAL_HEADER 88
#define SEAL_KEYS_MAX 16
struct seal_keys;
struct seal_writer;
struct seal_reader;
/* Private key directory contains active (32 hex chars) and <id>.key (64 hex).
 * Creation and rotation never remove older keys. No key material is logged. */
bool seal_keys_generate(const char *directory,bool rotate,char id[33]);
struct seal_keys *seal_keys_load(const char *directory);
void seal_keys_free(struct seal_keys *);
const char *seal_keys_active(const struct seal_keys *);
uint64_t seal_size(uint64_t plaintext);
/* Independent authenticated capsule, same MVSEAL01 format, at most one block.
 * Used for resumable server uploads so even uncommitted bytes are encrypted.
 * Output capacity must be SEAL_HEADER + plaintext bytes + 16. */
bool seal_capsule_encrypt(const struct seal_keys *,uint64_t identity,const void *,size_t,void *);
bool seal_capsule_decrypt(const struct seal_keys *,uint64_t identity,const void *,size_t,void *);
struct seal_writer *seal_writer_open(int fd,const struct seal_keys *,uint64_t id);
bool seal_writer_write(struct seal_writer *,const void *,size_t);
bool seal_writer_finish(struct seal_writer *);
void seal_writer_free(struct seal_writer *);
/* Authenticates header/identity/length before serving any bytes; each requested
 * chunk is authenticated before plaintext is copied to the caller. */
struct seal_reader *seal_reader_open(int fd,const struct seal_keys *,uint64_t id,uint64_t bytes);
int seal_reader_read(struct seal_reader *,uint64_t offset,void *,size_t);
void seal_reader_free(struct seal_reader *);
#endif
