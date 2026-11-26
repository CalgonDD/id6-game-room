#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "check") == 0)
        return 0;

    if (argc == 3 && strcmp(argv[1], "unlock") == 0) {
        if (rmdir(argv[2]) == 0)
            return 0;
        perror(argv[2]);
        return 1;
    }

    if (argc == 4 && strcmp(argv[1], "move") == 0) {
        struct stat destination;
        if (lstat(argv[3], &destination) == 0) {
            fprintf(stderr, "Destination already exists: %s\n", argv[3]);
            return 1;
        }
        if (errno != ENOENT) {
            perror(argv[3]);
            return 1;
        }
        if (rename(argv[2], argv[3]) == 0)
            return 0;
        fprintf(stderr, "Could not rename %s to %s: %s\n",
                argv[2], argv[3], strerror(errno));
        return 1;
    }

    /* hash PATH: 64-bit FNV-1a of the file's bytes as 16 hex digits (the games installer compares game files
     * already on the car with its own copies, so unchanged ones are not uploaded again). */
    if (argc == 3 && strcmp(argv[1], "hash") == 0) {
        static unsigned char buf[65536];
        unsigned long long h = 14695981039346656037ULL; /* FNV-1a 64 offset basis */
        size_t got, i;
        FILE *in = fopen(argv[2], "rb");
        if (!in) {
            perror(argv[2]);
            return 1;
        }
        while ((got = fread(buf, 1, sizeof buf, in)) > 0)
            for (i = 0; i < got; ++i) {
                h ^= buf[i];
                h *= 1099511628211ULL;
            }
        if (ferror(in)) {
            perror(argv[2]);
            fclose(in);
            return 1;
        }
        fclose(in);
        fprintf(stdout, "%016llx\n", h);
        return 0;
    }

    /* copy SOURCE DEST: copies a file to a new DEST (never over an existing one), flushed to the disk before it
     * returns. Used to move a staged game file onto the hard drive while that is briefly writable. */
    if (argc == 4 && strcmp(argv[1], "copy") == 0) {
        static unsigned char buf[65536];
        struct stat destination;
        size_t got;
        FILE *in, *out;
        if (lstat(argv[3], &destination) == 0) {
            fprintf(stderr, "Destination already exists: %s\n", argv[3]);
            return 1;
        }
        in = fopen(argv[2], "rb");
        if (!in) {
            perror(argv[2]);
            return 1;
        }
        out = fopen(argv[3], "wb");
        if (!out) {
            perror(argv[3]);
            fclose(in);
            return 1;
        }
        while ((got = fread(buf, 1, sizeof buf, in)) > 0)
            if (fwrite(buf, 1, got, out) != got) {
                perror(argv[3]);
                fclose(in);
                fclose(out);
                remove(argv[3]);
                return 1;
            }
        if (ferror(in) || fflush(out) != 0 || fsync(fileno(out)) != 0) {
            perror(argv[3]);
            fclose(in);
            fclose(out);
            remove(argv[3]);
            return 1;
        }
        fclose(in);
        if (fclose(out) != 0) {
            perror(argv[3]);
            remove(argv[3]);
            return 1;
        }
        return 0;
    }

    /* size PATH: the byte count, for the installer's chunked uploads (the car's shell has no ls or wc). */
    if (argc == 3 && strcmp(argv[1], "size") == 0) {
        struct stat file;
        if (stat(argv[2], &file) != 0) {
            perror(argv[2]);
            return 1;
        }
        fprintf(stdout, "%lld\n", (long long)file.st_size);
        return 0;
    }

    fputs("Usage: gameroom-fileops check | unlock DIR | move SOURCE DEST | size PATH | hash PATH | copy SOURCE DEST\n", stderr);
    return 2;
}
