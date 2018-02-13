#ifndef _HASH_H_
#define _HASH_H_

#define BLOCKSIZE 8

// Hash manipulation helper functions
char *hash(FILE *f);
int check_hash(const char *hash1, const char *hash2);
#endif // _HASH_H_
