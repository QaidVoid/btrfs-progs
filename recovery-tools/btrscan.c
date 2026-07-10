// btrscan - find btrfs metadata blocks in a raw image by header fsid
// probe mode:  ./btrscan probe <image> <stride_bytes>       -> regions with hits
// dense mode:  ./btrscan dense <image> <start> <end>        -> CSV of all tree blocks
// calib mode:  ./btrscan calib <image>                      -> verify crc32c flavor on superblock
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <nmmintrin.h>

// fsid of the target filesystem: ea7a2489-6ced-4a72-8084-b034b98c2c9c
static const uint8_t FSID[16] = {0xea,0x7a,0x24,0x89,0x6c,0xed,0x4a,0x72,
                                 0x80,0x84,0xb0,0x34,0xb9,0x8c,0x2c,0x9c};

#define NODESIZE 16384
#define HDR_FSID 32
#define HDR_BYTENR 48
#define HDR_FLAGS 56
#define HDR_CTUUID 64
#define HDR_GEN 80
#define HDR_OWNER 88
#define HDR_NRITEMS 96
#define HDR_LEVEL 100

static inline uint64_t rd64(const uint8_t *p){ uint64_t v; memcpy(&v,p,8); return v; }
static inline uint32_t rd32(const uint8_t *p){ uint32_t v; memcpy(&v,p,4); return v; }

// hardware crc32c (Castagnoli), init ~0, final invert — btrfs flavor
static uint32_t crc32c_hw(uint32_t crc, const uint8_t *buf, size_t len){
    while (len >= 8){ crc = (uint32_t)_mm_crc32_u64(crc, rd64(buf)); buf += 8; len -= 8; }
    while (len--) crc = _mm_crc32_u8(crc, *buf++);
    return crc;
}
static uint32_t btrfs_csum(const uint8_t *data, size_t len){
    return crc32c_hw(~0u, data, len) ^ ~0u;
}

int main(int argc, char **argv){
    if (argc < 3){ fprintf(stderr,"usage: %s probe|dense|calib <image> ...\n", argv[0]); return 2; }
    const char *mode = argv[1];
    int fd = open(argv[2], O_RDONLY);
    if (fd < 0){ perror("open"); return 1; }
    off_t fsize = lseek(fd, 0, SEEK_END);

    if (!strcmp(mode,"calib")){
        // superblock at 65536, csum over bytes 32..4095 (sb is 4096)
        uint8_t sb[4096];
        if (pread(fd, sb, 4096, 65536) != 4096){ perror("pread sb"); return 1; }
        uint32_t stored = rd32(sb);
        uint32_t a = btrfs_csum(sb+32, 4096-32);
        uint32_t b = crc32c_hw(0, sb+32, 4096-32);
        printf("stored=0x%08x  invflavor=0x%08x  rawflavor=0x%08x\n", stored, a, b);
        printf(stored==a ? "MATCH: inverted flavor\n" : stored==b ? "MATCH: raw flavor\n" : "NO MATCH\n");
        return 0;
    }

    if (!strcmp(mode,"probe")){
        uint64_t stride = strtoull(argv[3],NULL,0);
        uint8_t buf[65536];
        uint64_t region_start = UINT64_MAX, region_last = 0;
        for (uint64_t off = 0; off + 65536 <= (uint64_t)fsize; off += stride){
            ssize_t r = pread(fd, buf, 65536, off);
            if (r < 65536) break;
            int hit = 0;
            for (int i = 0; i + NODESIZE <= 65536 || i == 0; i += NODESIZE){
                if (i + 4096 > 65536) break;
                if (!memcmp(buf + i + HDR_FSID, FSID, 16)){ hit = 1; break; }
            }
            if (hit){
                if (region_start == UINT64_MAX) region_start = off;
                else if (off - region_last > stride) {
                    printf("REGION %llu %llu\n",(unsigned long long)region_start,(unsigned long long)(region_last+stride));
                    fflush(stdout);
                    region_start = off;
                }
                region_last = off;
            }
            if ((off / stride) % 2048 == 0){
                fprintf(stderr,"probe: %llu GiB\n",(unsigned long long)(off>>30)); }
        }
        if (region_start != UINT64_MAX)
            printf("REGION %llu %llu\n",(unsigned long long)region_start,(unsigned long long)(region_last+stride));
        return 0;
    }

    if (!strcmp(mode,"dense")){
        uint64_t start = strtoull(argv[3],NULL,0), end = strtoull(argv[4],NULL,0);
        if (end > (uint64_t)fsize) end = fsize;
        start &= ~((uint64_t)NODESIZE-1);
        size_t BUFSZ = 16*1024*1024;
        uint8_t *buf = malloc(BUFSZ + NODESIZE);
        posix_fadvise(fd, start, end-start, POSIX_FADV_SEQUENTIAL);
        printf("physical,bytenr,owner,generation,level,nritems,flags,csum_ok\n");
        for (uint64_t off = start; off < end; off += BUFSZ){
            size_t want = (off + BUFSZ <= end) ? BUFSZ : (end - off);
            ssize_t r = pread(fd, buf, want, off);
            if (r <= 0) break;
            for (size_t i = 0; i + NODESIZE <= (size_t)r; i += NODESIZE){
                const uint8_t *b = buf + i;
                if (memcmp(b + HDR_FSID, FSID, 16)) continue;
                uint32_t stored = rd32(b);
                uint32_t calc = btrfs_csum(b + 32, NODESIZE - 32);
                printf("%llu,%llu,%llu,%llu,%u,%u,%llu,%d\n",
                    (unsigned long long)(off + i),
                    (unsigned long long)rd64(b + HDR_BYTENR),
                    (unsigned long long)rd64(b + HDR_OWNER),
                    (unsigned long long)rd64(b + HDR_GEN),
                    b[HDR_LEVEL], rd32(b + HDR_NRITEMS),
                    (unsigned long long)rd64(b + HDR_FLAGS),
                    stored == calc);
            }
            fflush(stdout);
        }
        free(buf);
        return 0;
    }
    fprintf(stderr,"unknown mode\n");
    return 2;
}
