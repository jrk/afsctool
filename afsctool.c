#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <fts.h>
#include <sys/xattr.h>
#include <hfs/hfs_format.h>
#include <unistd.h>
#include <zlib.h>

#include <CoreServices/CoreServices.h>

const char *sizeunit10_short[] = {"KB", "MB", "GB", "TB", "PB", "EB"};
const char *sizeunit10_long[] = {"kilobytes", "megabytes", "gigabytes", "terabytes", "petabytes", "exabytes"};
const long long int sizeunit10[] = {1000, 1000 * 1000, 1000 * 1000 * 1000, (long long int) 1000 * 1000 * 1000 * 1000, (long long int) 1000 * 1000 * 1000 * 1000 * 1000, (long long int) 1000 * 1000 * 1000 * 1000 * 1000 * 1000};
const char *sizeunit2_short[] = {"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
const char *sizeunit2_long[] = {"kibibytes", "mebibytes", "gibibytes", "tebibytes", "pebibytes", "exbibytes"};
const long long int sizeunit2[] = {1024, 1024 * 1024, 1024 * 1024 * 1024, (long long int) 1024 * 1024 * 1024 * 1024, (long long int) 1024 * 1024 * 1024 * 1024 * 1024, (long long int) 1024 * 1024 * 1024 * 1024 * 1024 * 1024};

struct folder_info
{
	long long int uncompressed_size;
	long long int uncompressed_size_rounded;
	long long int compressed_size;
	long long int compressed_size_rounded;
	long long int compattr_size;
	long long int total_size;
	long long int num_compressed;
	long long int num_files;
	long long int num_hard_link_files;
	long long int num_folders;
	long long int num_hard_link_folders;
	long long int maxSize;
	int print_info;
	int compressionlevel;
	double minSavings;
	bool print_files;
	bool compress_files;
	bool check_files;
	bool check_hard_links;
};

char* getSizeStr(long long int size, long long int size_rounded)
{
	static char sizeStr[90];
	int unit2, unit10;
	
	for (unit2 = 0; unit2 + 1 < sizeof(sizeunit2) && (size_rounded / sizeunit2[unit2 + 1]) > 0; unit2++);
	for (unit10 = 0; unit10 + 1 < sizeof(sizeunit10) && (size_rounded / sizeunit10[unit10 + 1]) > 0; unit10++);
	
	sprintf(sizeStr, "%lld bytes / ", size);

	switch (unit10)
	{
		case 0:
			sprintf(sizeStr, "%s%0.0f %s (%s) / ", sizeStr, (double) size_rounded / sizeunit10[unit10], sizeunit10_short[unit10], sizeunit10_long[unit10]);
			break;
		case 1:
			sprintf(sizeStr, "%s%.12g %s (%s) / ", sizeStr, (double) (((long long int) ((double) size_rounded / sizeunit10[unit10] * 100) + 5) / 10) / 10, sizeunit10_short[unit10], sizeunit10_long[unit10]);
			break;
		default:
			sprintf(sizeStr, "%s%0.12g %s (%s) / ", sizeStr,  (double) (((long long int) ((double) size_rounded / sizeunit10[unit10] * 1000) + 5) / 10) / 100, sizeunit10_short[unit10], sizeunit10_long[unit10]);
			break;
	}
	
	switch (unit2)
	{
		case 0:
			sprintf(sizeStr, "%s%0.0f %s (%s)", sizeStr, (double) size_rounded / sizeunit2[unit2], sizeunit2_short[unit2], sizeunit2_long[unit2]);
			break;
		case 1:
			sprintf(sizeStr, "%s%.12g %s (%s)", sizeStr, (double) (((long long int) ((double) size_rounded / sizeunit2[unit2] * 100) + 5) / 10) / 10, sizeunit2_short[unit2], sizeunit2_long[unit2]);
			break;
		default:
			sprintf(sizeStr, "%s%0.12g %s (%s)", sizeStr,  (double) (((long long int) ((double) size_rounded / sizeunit2[unit2] * 1000) + 5) / 10) / 100, sizeunit2_short[unit2], sizeunit2_long[unit2]);
			break;
	}
	
	return sizeStr;
}

void compressFile(const char *inFile, struct stat *inFileInfo, long long int maxSize, int compressionlevel, double minSavings, bool checkFiles)
{
	FILE *in;
	struct statfs fsInfo;
	unsigned int compblksize = 0x10000, numBlocks, outdecmpfsSize = 0;
	void *inBuf, *outBuf, *outBufBlock, *outdecmpfsBuf, *currBlock, *blockStart;
	long long int inBufPos, filesize = inFileInfo->st_size;
	unsigned long int cmpedsize;
	char *xattrnames, *curr_attr;
	ssize_t xattrnamesize;
	UInt32 cmpf = 0x636D7066;
	struct timeval times[2];
	
	times[0].tv_sec = inFileInfo->st_atimespec.tv_sec;
	times[0].tv_usec = inFileInfo->st_atimespec.tv_nsec / 1000;
	times[1].tv_sec = inFileInfo->st_mtimespec.tv_sec;
	times[1].tv_usec = inFileInfo->st_mtimespec.tv_nsec / 1000;
	
	if (statfs(inFile, &fsInfo) < 0)
		return;
	if (fsInfo.f_type != 17)
		return;
	
	if (!S_ISREG(inFileInfo->st_mode))
		return;
	if ((inFileInfo->st_flags & UF_COMPRESSED) != 0)
		return;
	if (filesize > maxSize && maxSize != 0)
		return;
	if (filesize == 0)
		return;
	
	if (chflags(inFile, UF_COMPRESSED | inFileInfo->st_flags) < 0 || chflags(inFile, inFileInfo->st_flags) < 0)
	{
		fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		return;
	}
	
	xattrnamesize = listxattr(inFile, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
	
	if (xattrnamesize > 0)
	{
		xattrnames = (char *) malloc(xattrnamesize);
		if (xattrnames == NULL)
		{
			fprintf(stderr, "%s: malloc error, unable to get file information\n", inFile);
			return;
		}
		if ((xattrnamesize = listxattr(inFile, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
		{
			fprintf(stderr, "%s: listxattr: %s\n", inFile, strerror(errno));
			free(xattrnames);
			return;
		}
		for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
		{
			if ((strcmp(curr_attr, "com.apple.ResourceFork") == 0 && strlen(curr_attr) == 22) ||
				(strcmp(curr_attr, "com.apple.decmpfs") == 0 && strlen(curr_attr) == 17))
				return;
		}
		free(xattrnames);
	}
	
	numBlocks = (filesize + compblksize - 1) / compblksize;
	if ((filesize + 0x13A + (numBlocks * 9)) > 2147483647)
		return;
	
	in = fopen(inFile, "r+");
	if (in == NULL)
	{
		fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
		return;
	}
	inBuf = malloc(filesize);
	if (inBuf == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate input buffer\n", inFile);
		fclose(in);
		utimes(inFile, times);
		return;
	}
	if (fread(inBuf, filesize, 1, in) != 1)
	{
		fprintf(stderr, "%s: Error reading file\n", inFile);
		fclose(in);
		utimes(inFile, times);
		free(inBuf);
		return;
	}
	fclose(in);
	outBuf = malloc(filesize + 0x13A + (numBlocks * 9));
	if (outBuf == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate output buffer\n", inFile);
		utimes(inFile, times);
		free(inBuf);
		return;
	}
	outdecmpfsBuf = malloc(3802);
	if (outdecmpfsBuf == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate xattr buffer\n", inFile);
		utimes(inFile, times);
		free(inBuf);
		free(outBuf);
		return;
	}
	outBufBlock = malloc(compressBound(compblksize));
	if (outBufBlock == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate compression buffer\n", inFile);
		utimes(inFile, times);
		free(inBuf);
		free(outBuf);
		free(outdecmpfsBuf);
		return;
	}
	*(UInt32 *) outdecmpfsBuf = EndianU32_NtoL(cmpf);
	*(UInt32 *) (outdecmpfsBuf + 4) = EndianU32_NtoL(4);
	*(UInt64 *) (outdecmpfsBuf + 8) = EndianU64_NtoL(filesize);
	outdecmpfsSize = 0x10;
	*(UInt32 *) outBuf = EndianU32_NtoB(0x100);
	*(UInt32 *) (outBuf + 12) = EndianU32_NtoB(0x32);
	memset(outBuf + 16, 0, 0xF0);
	blockStart = outBuf + 0x104;
	*(UInt32 *) blockStart = EndianU32_NtoL(numBlocks);
	currBlock = blockStart + 0x4 + (numBlocks * 8);
	for (inBufPos = 0; inBufPos < filesize; inBufPos += compblksize, currBlock += cmpedsize)
	{
		cmpedsize = compressBound(compblksize);
		if (compress2(outBufBlock, &cmpedsize, inBuf + inBufPos, ((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos, compressionlevel) != Z_OK)
		{
			utimes(inFile, times);
			free(inBuf);
			free(outBuf);
			free(outdecmpfsBuf);
			free(outBufBlock);
			return;
		}
		if (cmpedsize > (((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos))
		{
			*(unsigned char *) outBufBlock = 0xFF;
			memcpy(outBufBlock + 1, inBuf + inBufPos, ((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos);
			cmpedsize = ((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos;
			cmpedsize++;
		}
		if (((cmpedsize + outdecmpfsSize) <= 3802) && (numBlocks <= 1))
		{
			*(UInt32 *) (outdecmpfsBuf + 4) = EndianU32_NtoL(3);
			memcpy(outdecmpfsBuf + outdecmpfsSize, outBufBlock, cmpedsize);
			outdecmpfsSize += cmpedsize;
			break;
		}
		memcpy(currBlock, outBufBlock, cmpedsize);
		*(UInt32 *) (blockStart + ((inBufPos / compblksize) * 8) + 0x4) = EndianU32_NtoL(currBlock - blockStart);
		*(UInt32 *) (blockStart + ((inBufPos / compblksize) * 8) + 0x8) = EndianU32_NtoL(cmpedsize);
	}
	
	if (EndianU32_LtoN(*(UInt32 *) (outdecmpfsBuf + 4)) == 4)
	{
		if ((((double) (currBlock - outBuf + 50) / filesize) >= (1.0 - minSavings / 100) && minSavings != 0.0) ||
			currBlock - outBuf + 50 >= filesize)
		{
			utimes(inFile, times);
			free(inBuf);
			free(outBuf);
			free(outdecmpfsBuf);
			free(outBufBlock);
			return;
		}
		*(UInt32 *) (outBuf + 4) = EndianU32_NtoB(currBlock - outBuf);
		*(UInt32 *) (outBuf + 8) = EndianU32_NtoB(currBlock - outBuf - 0x100);
		*(UInt32 *) (blockStart - 4) = EndianU32_NtoB(currBlock - outBuf - 0x104);
		memset(currBlock, 0, 24);
		*(UInt16 *) (currBlock + 24) = EndianU16_NtoB(0x1C);
		*(UInt16 *) (currBlock + 26) = EndianU16_NtoB(0x32);
		*(UInt16 *) (currBlock + 28) = 0;
		*(UInt32 *) (currBlock + 30) = EndianU32_NtoB(cmpf);
		*(UInt32 *) (currBlock + 34) = EndianU32_NtoB(0xA);
		*(UInt64 *) (currBlock + 38) = EndianU64_NtoL(0xFFFF0100);
		*(UInt32 *) (currBlock + 46) = 0;
		if (setxattr(inFile, "com.apple.ResourceFork", outBuf, currBlock - outBuf + 50, 0, XATTR_NOFOLLOW | XATTR_CREATE) < 0)
		{
			fprintf(stderr, "%s: setxattr: %s\n", inFile, strerror(errno));
			free(inBuf);
			free(outBuf);
			free(outdecmpfsBuf);
			free(outBufBlock);
			return;
		}
	}
	if (setxattr(inFile, "com.apple.decmpfs", outdecmpfsBuf, outdecmpfsSize, 0, XATTR_NOFOLLOW | XATTR_CREATE) < 0)
	{
		fprintf(stderr, "%s: setxattr: %s\n", inFile, strerror(errno));
		free(inBuf);
		free(outBuf);
		free(outdecmpfsBuf);
		free(outBufBlock);
		return;
	}
	in = fopen(inFile, "w");
	if (in == NULL)
	{
		fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
		return;
	}
	fclose(in);
	if (chflags(inFile, UF_COMPRESSED | inFileInfo->st_flags) < 0)
	{
		fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		if (removexattr(inFile, "com.apple.decmpfs", XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
		{
			fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
		}
		if (EndianU32_LtoN(*(UInt32 *) (outdecmpfsBuf + 4)) == 4 &&
			removexattr(inFile, "com.apple.ResourceFork", XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
		{
			fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
		}
		in = fopen(inFile, "w");
		if (in == NULL)
		{
			free(inBuf);
			free(outBuf);
			free(outdecmpfsBuf);
			free(outBufBlock);
			fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
			return;
		}
		if (fwrite(inBuf, filesize, 1, in) != 1)
		{
			free(inBuf);
			free(outBuf);
			free(outdecmpfsBuf);
			free(outBufBlock);
			fprintf(stderr, "%s: Error writing to file\n", inFile);
			return;
		}
		fclose(in);
		utimes(inFile, times);
		return;
	}
	if (checkFiles)
	{
		lstat(inFile, inFileInfo);
		in = fopen(inFile, "r");
		if (in == NULL)
		{
			fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
			return;
		}
		if (inFileInfo->st_size != filesize || 
			fread(outBuf, filesize, 1, in) != 1 ||
			memcmp(outBuf, inBuf, filesize) != 0)
		{
			fclose(in);
			printf("%s: Compressed file check failed, reverting file changes\n", inFile);
			if (chflags(inFile, (~UF_COMPRESSED) & inFileInfo->st_flags) < 0)
			{
				free(inBuf);
				free(outBuf);
				free(outdecmpfsBuf);
				free(outBufBlock);
				fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
				return;
			}
			if (removexattr(inFile, "com.apple.decmpfs", XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
			{
				fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
			}
			if (EndianU32_LtoN(*(UInt32 *) (outdecmpfsBuf + 4)) == 4 && 
				removexattr(inFile, "com.apple.ResourceFork", XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
			{
				fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
			}
			in = fopen(inFile, "w");
			if (in == NULL)
			{
				free(inBuf);
				free(outBuf);
				free(outdecmpfsBuf);
				free(outBufBlock);
				fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
				return;
			}
			if (fwrite(inBuf, filesize, 1, in) != 1)
			{
				free(inBuf);
				free(outBuf);
				free(outdecmpfsBuf);
				free(outBufBlock);
				fprintf(stderr, "%s: Error writing to file\n", inFile);
				return;
			}
		}
		fclose(in);
	}
	utimes(inFile, times);
	free(inBuf);
	free(outBuf);
	free(outdecmpfsBuf);
	free(outBufBlock);
}

void decompressFile(const char *inFile, struct stat *inFileInfo)
{
	FILE *in;
	int uncmpret;
	unsigned int compblksize = 0x10000, numBlocks, currBlock;
	long long int filesize;
	unsigned long int uncmpedsize;
	void *inBuf = NULL, *outBuf, *indecmpfsBuf = NULL, *blockStart;
	char *xattrnames, *curr_attr;
	ssize_t xattrnamesize, indecmpfsLen = 0, inRFLen = 0, getxattrret, RFpos = 0;
	struct timeval times[2];
	
	times[0].tv_sec = inFileInfo->st_atimespec.tv_sec;
	times[0].tv_usec = inFileInfo->st_atimespec.tv_nsec / 1000;
	times[1].tv_sec = inFileInfo->st_mtimespec.tv_sec;
	times[1].tv_usec = inFileInfo->st_mtimespec.tv_nsec / 1000;
	
	if (!S_ISREG(inFileInfo->st_mode))
		return;
	if ((inFileInfo->st_flags & UF_COMPRESSED) == 0)
		return;
	
	xattrnamesize = listxattr(inFile, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
	
	if (xattrnamesize > 0)
	{
		xattrnames = (char *) malloc(xattrnamesize);
		if (xattrnames == NULL)
		{
			fprintf(stderr, "%s: malloc error, unable to get file information\n", inFile);
			return;
		}
		if ((xattrnamesize = listxattr(inFile, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
		{
			fprintf(stderr, "%s: listxattr: %s\n", inFile, strerror(errno));
			free(xattrnames);
			return;
		}
		for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
		{
			if (strcmp(curr_attr, "com.apple.ResourceFork") == 0 && strlen(curr_attr) == 22)
			{
				inRFLen = getxattr(inFile, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
				if (inRFLen < 0)
				{
					fprintf(stderr, "%s: getxattr: %s\n", inFile, strerror(errno));
					free(xattrnames);
					return;
				}
				if (inRFLen != 0)
				{
					inBuf = malloc(inRFLen);
					if (inBuf == NULL)
					{
						fprintf(stderr, "%s: malloc error, unable to allocate input buffer\n", inFile);
						return;
					}
					do
					{
						getxattrret = getxattr(inFile, curr_attr, inBuf + RFpos, inRFLen - RFpos, RFpos, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
						if (getxattrret < 0)
						{
							fprintf(stderr, "getxattr: %s\n", strerror(errno));
							free(xattrnames);
							return;
						}
						RFpos += getxattrret;
					} while (RFpos < inRFLen && getxattrret > 0);
				}
			}
			if (strcmp(curr_attr, "com.apple.decmpfs") == 0 && strlen(curr_attr) == 17)
			{
				indecmpfsLen = getxattr(inFile, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
				if (indecmpfsLen < 0)
				{
					fprintf(stderr, "%s: getxattr: %s\n", inFile, strerror(errno));
					free(xattrnames);
					return;
				}
				if (indecmpfsLen != 0)
				{
					indecmpfsBuf = malloc(indecmpfsLen);
					if (indecmpfsBuf == NULL)
					{
						fprintf(stderr, "%s: malloc error, unable to allocate xattr buffer\n", inFile);
						utimes(inFile, times);
						free(inBuf);
						return;
					}
					if (indecmpfsLen != 0)
					{
						indecmpfsBuf = malloc(indecmpfsLen);
						if (indecmpfsBuf == NULL)
						{
							fprintf(stderr, "%s: malloc error, unable to get file information\n", inFile);
							return;
						}
						indecmpfsLen = getxattr(inFile, curr_attr, indecmpfsBuf, indecmpfsLen, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
						if (indecmpfsLen < 0)
						{
							fprintf(stderr, "getxattr: %s\n", strerror(errno));
							free(xattrnames);
							return;
						}
					}
				}
			}
		}
		free(xattrnames);
	}
	
	if (indecmpfsBuf == NULL)
	{
		fprintf(stderr, "%s: Decompression failed; file flags indicate file is compressed but it does not have a com.apple.decmpfs extended attribute\n", inFile);
		if (inBuf != NULL)
			free(inBuf);
		if (indecmpfsBuf != NULL)
			free(indecmpfsBuf);
		return;
	}
	if (indecmpfsLen < 0x10)
	{
		fprintf(stderr, "%s: Decompression failed; extended attribute com.apple.decmpfs is only %ld bytes (it is required to have a 16 byte header)\n", inFile, indecmpfsLen);
		if (inBuf != NULL)
			free(inBuf);
		if (indecmpfsBuf != NULL)
			free(indecmpfsBuf);
		return;
	}
	
	filesize = EndianU64_LtoN(*(UInt64 *) (indecmpfsBuf + 8));
	if (filesize == 0)
	{
		fprintf(stderr, "%s: Decompression failed; file size given in header is 0\n", inFile);
		if (inBuf != NULL)
			free(inBuf);
		if (indecmpfsBuf != NULL)
			free(indecmpfsBuf);
		return;
	}
	outBuf = malloc(filesize);
	if (outBuf == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate output buffer\n", inFile);
		if (inBuf != NULL)
			free(inBuf);
		if (indecmpfsBuf != NULL)
			free(indecmpfsBuf);
		return;
	}
	
	if (EndianU32_LtoN(*(UInt32 *) (indecmpfsBuf + 4)) == 4)
	{
		if (inBuf == NULL)
		{
			fprintf(stderr, "%s: Decompression failed; resource fork required for compression type 4 but none exists\n", inFile);
			if (inBuf != NULL)
				free(inBuf);
			if (indecmpfsBuf != NULL)
				free(indecmpfsBuf);
			free(outBuf);
			return;
		}
		if (inRFLen < 0x13A ||
			inRFLen < EndianU32_BtoN(*(UInt32 *) inBuf) + 0x4)
		{
			fprintf(stderr, "%s: Decompression failed; resource fork data is incomplete\n", inFile);
			if (inBuf != NULL)
				free(inBuf);
			if (indecmpfsBuf != NULL)
				free(indecmpfsBuf);
			free(outBuf);
			return;
		}
		
		blockStart = inBuf + EndianU32_BtoN(*(UInt32 *) inBuf) + 0x4;
		numBlocks = EndianU32_NtoL(*(UInt32 *) blockStart);
		
		if (inRFLen < EndianU32_BtoN(*(UInt32 *) inBuf) + 0x3A + (numBlocks * 8))
		{
			fprintf(stderr, "%s: Decompression failed; resource fork data is incomplete\n", inFile);
			if (inBuf != NULL)
				free(inBuf);
			if (indecmpfsBuf != NULL)
				free(indecmpfsBuf);
			free(outBuf);
			return;
		}
		if (compblksize * (numBlocks - 1) + (filesize % compblksize) > filesize)
		{
			fprintf(stderr, "%s: Decompression failed; file size given in header is incorrect\n", inFile);
			if (inBuf != NULL)
				free(inBuf);
			if (indecmpfsBuf != NULL)
				free(indecmpfsBuf);
			free(outBuf);
			return;
		}
		for (currBlock = 0; currBlock < numBlocks; currBlock++)
		{
			if (blockStart + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x4 + (currBlock * 8))) + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x8 + (currBlock * 8))) > inBuf + inRFLen)
			{
				fprintf(stderr, "%s: Decompression failed; resource fork data is incomplete\n", inFile);
				if (inBuf != NULL)
					free(inBuf);
				if (indecmpfsBuf != NULL)
					free(indecmpfsBuf);
				free(outBuf);
				return;
			}
			if (currBlock + 1 != numBlocks)
				uncmpedsize = compblksize;
			else
				uncmpedsize = (filesize - (currBlock * compblksize) < compblksize) ? filesize - (currBlock * compblksize) : compblksize;
			if ((compblksize * currBlock) + uncmpedsize > filesize)
			{
				fprintf(stderr, "%s: Decompression failed; file size given in header is incorrect\n", inFile);
				if (inBuf != NULL)
					free(inBuf);
				if (indecmpfsBuf != NULL)
					free(indecmpfsBuf);
				free(outBuf);
				return;
			}
			if ((*(unsigned char *) (blockStart + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x4 + (currBlock * 8))))) == 0xFF)
			{
				uncmpedsize = EndianU32_LtoN(*(UInt32 *) (blockStart + 0x8 + (currBlock * 8))) - 1;
				memcpy(outBuf + (compblksize * currBlock), blockStart + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x4 + (currBlock * 8))) + 1, uncmpedsize);
			}
			else
			{
				if ((uncmpret = uncompress(outBuf + (compblksize * currBlock), &uncmpedsize, blockStart + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x4 + (currBlock * 8))), EndianU32_LtoN(*(UInt32 *) (blockStart + 0x8 + (currBlock * 8))))) != Z_OK)
				{
					if (uncmpret == Z_BUF_ERROR)
					{
						fprintf(stderr, "%s: Decompression failed; uncompressed data block too large\n", inFile);
						if (inBuf != NULL)
							free(inBuf);
						if (indecmpfsBuf != NULL)
							free(indecmpfsBuf);
						free(outBuf);
						return;
					}
					else if (uncmpret == Z_DATA_ERROR)
					{
						fprintf(stderr, "%s: Decompression failed; compressed data block is corrupted\n", inFile);
						if (inBuf != NULL)
							free(inBuf);
						if (indecmpfsBuf != NULL)
							free(indecmpfsBuf);
						free(outBuf);
						return;
					}
					else if (uncmpret == Z_MEM_ERROR)
					{
						fprintf(stderr, "%s: Decompression failed; out of memory\n", inFile);
						if (inBuf != NULL)
							free(inBuf);
						if (indecmpfsBuf != NULL)
							free(indecmpfsBuf);
						free(outBuf);
						return;
					}
					else
					{
						fprintf(stderr, "%s: Decompression failed; an error occurred during decompression\n", inFile);
						if (inBuf != NULL)
							free(inBuf);
						if (indecmpfsBuf != NULL)
							free(indecmpfsBuf);
						free(outBuf);
						return;
					}
				}
			}
			if (uncmpedsize != ((filesize - (currBlock * compblksize) < compblksize) ? filesize - (currBlock * compblksize) : compblksize))
			{
				fprintf(stderr, "%s: Decompression failed; uncompressed data block too small\n", inFile);
				if (inBuf != NULL)
					free(inBuf);
				if (indecmpfsBuf != NULL)
					free(indecmpfsBuf);
				free(outBuf);
				return;
			}
		}
	}
	else if (EndianU32_LtoN(*(UInt32 *) (indecmpfsBuf + 4)) == 3)
	{
		if (indecmpfsLen == 0x10)
		{
			fprintf(stderr, "%s: Decompression failed; compression type 3 expects compressed data in extended attribute com.apple.decmpfs but none exists\n", inFile);
			if (inBuf != NULL)
				free(inBuf);
			if (indecmpfsBuf != NULL)
				free(indecmpfsBuf);
			free(outBuf);
			return;
		}
		uncmpedsize = filesize;
		if ((*(unsigned char *) (indecmpfsBuf + 0x10)) == 0xFF)
		{
			uncmpedsize = indecmpfsLen - 0x11;
			memcpy(outBuf, indecmpfsBuf + 0x11, uncmpedsize);
		}
		else
		{
			if ((uncmpret = uncompress(outBuf, &uncmpedsize, indecmpfsBuf + 0x10, indecmpfsLen - 0x10)) != Z_OK)
			{
				if (uncmpret == Z_BUF_ERROR)
				{
					fprintf(stderr, "%s: Decompression failed; uncompressed data too large\n", inFile);
					if (inBuf != NULL)
						free(inBuf);
					if (indecmpfsBuf != NULL)
						free(indecmpfsBuf);
					free(outBuf);
					return;
				}
				else if (uncmpret == Z_DATA_ERROR)
				{
					fprintf(stderr, "%s: Decompression failed; compressed data is corrupted\n", inFile);
					if (inBuf != NULL)
						free(inBuf);
					if (indecmpfsBuf != NULL)
						free(indecmpfsBuf);
					free(outBuf);
					return;
				}
				else if (uncmpret == Z_MEM_ERROR)
				{
					fprintf(stderr, "%s: Decompression failed; out of memory\n", inFile);
					if (inBuf != NULL)
						free(inBuf);
					if (indecmpfsBuf != NULL)
						free(indecmpfsBuf);
					free(outBuf);
					return;
				}
				else
				{
					fprintf(stderr, "%s: Decompression failed; an error occurred during decompression\n", inFile);
					if (inBuf != NULL)
						free(inBuf);
					if (indecmpfsBuf != NULL)
						free(indecmpfsBuf);
					free(outBuf);
					return;
				}
			}
		}
		if (uncmpedsize != filesize)
		{
			fprintf(stderr, "%s: Decompression failed; uncompressed data block too small\n", inFile);
			if (inBuf != NULL)
				free(inBuf);
			if (indecmpfsBuf != NULL)
				free(indecmpfsBuf);
			free(outBuf);
			return;
		}
	}
	else
	{
		fprintf(stderr, "%s: Decompression failed; unknown compression type %u\n", inFile, (unsigned int) EndianU32_LtoN(*(UInt32 *) (indecmpfsBuf + 4)));
		if (inBuf != NULL)
			free(inBuf);
		if (indecmpfsBuf != NULL)
			free(indecmpfsBuf);
		free(outBuf);
		return;
	}
	
	if (chflags(inFile, (~UF_COMPRESSED) & inFileInfo->st_flags) < 0)
	{
		fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		if (inBuf != NULL)
			free(inBuf);
		if (indecmpfsBuf != NULL)
			free(indecmpfsBuf);
		free(outBuf);
		return;
	}
	
	in = fopen(inFile, "r+");
	if (in == NULL)
	{
		fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
		if (inBuf != NULL)
			free(inBuf);
		if (indecmpfsBuf != NULL)
			free(indecmpfsBuf);
		free(outBuf);
		utimes(inFile, times);
		return;
	}
	
	if (fwrite(outBuf, filesize, 1, in) != 1)
	{
		fclose(in);
		if (chflags(inFile, UF_COMPRESSED | inFileInfo->st_flags) < 0)
		{
			fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		}
		if (inBuf != NULL)
			free(inBuf);
		if (indecmpfsBuf != NULL)
			free(indecmpfsBuf);
		free(outBuf);
		utimes(inFile, times);
		fprintf(stderr, "%s: Error writing to file\n", inFile);
		return;
	}
	
	fclose(in);
	
	if (removexattr(inFile, "com.apple.decmpfs", XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
	{
		fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
	}
	if (EndianU32_LtoN(*(UInt32 *) (indecmpfsBuf + 4)) == 4 && 
		removexattr(inFile, "com.apple.ResourceFork", XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
	{
		fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
	}
	
	if (inBuf != NULL)
		free(inBuf);
	if (indecmpfsBuf != NULL)
		free(indecmpfsBuf);
	free(outBuf);
	utimes(inFile, times);
}

bool checkForHardLink(const char *filepath, const struct stat *fileInfo, const struct folder_info *folderinfo)
{
	static ino_t *hardLinks = NULL;
	static char **paths = NULL, *list_item;
	static long int currSize = 0, numLinks = 0;
	long int right_pos, left_pos = 0, curr_pos = 1;
	
	if (fileInfo != NULL && fileInfo->st_nlink > 1)
	{
		if (hardLinks == NULL)
		{
			currSize = 1;
			hardLinks = (ino_t *) malloc(currSize * sizeof(ino_t));
			if (hardLinks == NULL)
			{
				fprintf(stderr, "Malloc error allocating memory for list of file hard links, exiting...\n");
				exit(-1);
			}
			paths = (char **) malloc(currSize * sizeof(char *));
			if (paths == NULL)
			{
				fprintf(stderr, "Malloc error allocating memory for list of file hard links, exiting...\n");
				exit(-1);
			}
		}
		
		if (numLinks > 0)
		{
			left_pos = 0;
			right_pos = numLinks + 1;
			
			while (hardLinks[curr_pos-1] != fileInfo->st_ino)
			{
				curr_pos = (right_pos - left_pos) / 2;
				if (curr_pos == 0) break;
				curr_pos += left_pos;
				if (hardLinks[curr_pos-1] > fileInfo->st_ino)
					right_pos = curr_pos;
				else if (hardLinks[curr_pos-1] < fileInfo->st_ino)
					left_pos = curr_pos;
			}
			if (curr_pos != 0 && hardLinks[curr_pos-1] == fileInfo->st_ino)
			{
				if (strcmp(filepath, paths[curr_pos-1]) != 0 || strlen(filepath) != strlen(paths[curr_pos-1]))
				{
					if (folderinfo->print_info > 1)
						printf("%s: skipping, hard link to this %s exists at %s\n", filepath, (fileInfo->st_mode & S_IFDIR) ? "folder" : "file", paths[curr_pos-1]);
					return TRUE;
				}
				else
					return FALSE;
			}
		}
		if (currSize < numLinks + 1)
		{
			currSize *= 2;
			hardLinks = realloc(hardLinks, currSize * sizeof(ino_t));
			if (hardLinks == NULL)
			{
				fprintf(stderr, "Malloc error allocating memory for list of file hard links, exiting...\n");
				exit(-1);
			}
			paths = realloc(paths, currSize * sizeof(char *));
			if (paths == NULL)
			{
				fprintf(stderr, "Malloc error allocating memory for list of file hard links, exiting...\n");
				exit(-1);
			}
		}
		if ((numLinks != 0) && ((numLinks - 1) >= left_pos))
		{
			memmove(&hardLinks[left_pos+1], &hardLinks[left_pos], (numLinks - left_pos) * sizeof(ino_t));
			if (paths != NULL)
				memmove(&paths[left_pos+1], &paths[left_pos], (numLinks - left_pos) * sizeof(char *));
		}
		hardLinks[left_pos] = fileInfo->st_ino;
		list_item = (char *) malloc(strlen(filepath) + 1);
		strcpy(list_item, filepath);
		paths[left_pos] = list_item;
		numLinks++;
	}
	else if (fileInfo == NULL && hardLinks != NULL)
	{
		free(hardLinks);
		hardLinks = NULL;
		currSize = 0;
		numLinks = 0;
		if (paths != NULL)
		{
			for (curr_pos = 0; curr_pos < numLinks; curr_pos++)
				free(paths[curr_pos]);
			free(paths);
		}
	}
	return FALSE;
}

void printFileInfo(const char *filepath, struct stat *fileinfo, bool appliedcomp)
{
	char *xattrnames, *curr_attr;
	ssize_t xattrnamesize, xattrssize = 0, xattrsize, RFsize = 0, compattrsize = 0;
	long long int filesize, filesize_rounded;
	int numxattrs = 0, numhiddenattr = 0;
	bool hasRF = FALSE;
	
	printf("%s:\n", filepath);
	
	xattrnamesize = listxattr(filepath, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
	
	if (xattrnamesize > 0)
	{
		xattrnames = (char *) malloc(xattrnamesize);
		if (xattrnames == NULL)
		{
			fprintf(stderr, "malloc error, unable to get file information\n");
			return;
		}
		if ((xattrnamesize = listxattr(filepath, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
		{
			fprintf(stderr, "listxattr: %s\n", strerror(errno));
			free(xattrnames);
			return;
		}
		for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
		{
			xattrsize = getxattr(filepath, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
			if (xattrsize < 0)
			{
				fprintf(stderr, "getxattr: %s\n", strerror(errno));
				free(xattrnames);
				return;
			}
			numxattrs++;
			if (strcmp(curr_attr, "com.apple.ResourceFork") == 0 && strlen(curr_attr) == 22)
			{
				RFsize += xattrsize;
				hasRF = TRUE;
				numhiddenattr++;
			}
			else if (strcmp(curr_attr, "com.apple.decmpfs") == 0 && strlen(curr_attr) == 17)
			{
				compattrsize += xattrsize;
				numhiddenattr++;
			}
			else
				xattrssize += xattrsize;
		}
		free(xattrnames);
	}
	
	if ((fileinfo->st_flags & UF_COMPRESSED) == 0)
	{
		if (appliedcomp)
			printf("Unable to compress file.\n");
		else
			printf("File is not HFS+ compressed.\n");
		if (hasRF)
		{
			printf("File data fork size: %lld bytes\n", fileinfo->st_size);
			printf("File resource fork size: %ld bytes\n", RFsize);
			filesize_rounded = filesize = fileinfo->st_size;
			filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
			filesize += RFsize;
			filesize_rounded += RFsize;
			filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
			printf("File size (data fork + resource fork; reported size by Mac OS X Finder): %s\n", getSizeStr(filesize, filesize_rounded));
		}
		else
		{
			filesize_rounded = filesize = fileinfo->st_size;
			filesize_rounded += (filesize % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize % fileinfo->st_blksize) : 0;
			printf("File data fork size (reported size by Mac OS X Finder): %s\n", getSizeStr(filesize, filesize_rounded));
		}
		printf("Number of extended attributes: %d\n", numxattrs - numhiddenattr);
		printf("Total size of extended attribute data: %ld bytes\n", xattrssize);
		printf("Appoximate overhead of extended attributes: %ld bytes\n", ((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey));
		filesize = fileinfo->st_size;
		filesize += (filesize % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize % fileinfo->st_blksize) : 0;
		filesize += RFsize;
		filesize += (filesize % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize % fileinfo->st_blksize) : 0;
		filesize += compattrsize + xattrssize + (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
		printf("Appoximate total file size (data fork + resource fork + EA + EA overhead + file overhead): %s\n", getSizeStr(filesize, filesize));
	}
	else
	{
		if (!appliedcomp)
			printf("File is HFS+ compressed.\n");
		filesize = fileinfo->st_size;
		printf("File size (uncompressed data fork; reported size by Mac OS 10.6+ Finder): %s\n", getSizeStr(filesize, filesize));
		filesize_rounded = filesize = RFsize;
		filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
		printf("File size (compressed data fork - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n", getSizeStr(filesize, filesize_rounded));
		filesize_rounded = filesize = RFsize;
		filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
		filesize += compattrsize;
		filesize_rounded += compattrsize;
		printf("File size (compressed data fork): %s\n", getSizeStr(filesize, filesize_rounded));
		printf("Compression savings: %0.1f%%\n", (1.0 - (((double) RFsize + compattrsize) / fileinfo->st_size)) * 100.0);
		printf("Number of extended attributes: %d\n", numxattrs - numhiddenattr);
		printf("Total size of extended attribute data: %ld bytes\n", xattrssize);
		printf("Appoximate overhead of extended attributes: %ld bytes\n", ((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey));
		filesize = RFsize;
		filesize += (filesize % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize % fileinfo->st_blksize) : 0;
		filesize += compattrsize + xattrssize + (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
		printf("Appoximate total file size (compressed data fork + EA + EA overhead + file overhead): %s\n", getSizeStr(filesize, filesize));
	}
}

void process_file(const char *filepath, struct stat *fileinfo, struct folder_info *folderinfo)
{
	char *xattrnames, *curr_attr;
	ssize_t xattrnamesize, xattrssize = 0, xattrsize, RFsize = 0, compattrsize = 0;
	long long int filesize, filesize_rounded;
	int numxattrs = 0, numhiddenattr = 0;
	bool hasRF = FALSE;
	
	xattrnamesize = listxattr(filepath, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
	
	if (xattrnamesize > 0)
	{
		xattrnames = (char *) malloc(xattrnamesize);
		if (xattrnames == NULL)
		{
			fprintf(stderr, "malloc error, unable to get file information\n");
			return;
		}
		if ((xattrnamesize = listxattr(filepath, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
		{
			fprintf(stderr, "listxattr: %s\n", strerror(errno));
			free(xattrnames);
			return;
		}
		for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
		{
			xattrsize = getxattr(filepath, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
			if (xattrsize < 0)
			{
				fprintf(stderr, "getxattr: %s\n", strerror(errno));
				free(xattrnames);
				return;
			}
			numxattrs++;
			if (strcmp(curr_attr, "com.apple.ResourceFork") == 0 && strlen(curr_attr) == 22)
			{
				RFsize += xattrsize;
				hasRF = TRUE;
				numhiddenattr++;
			}
			else if (strcmp(curr_attr, "com.apple.decmpfs") == 0 && strlen(curr_attr) == 17)
			{
				compattrsize += xattrsize;
				numhiddenattr++;
			}
			else
				xattrssize += xattrsize;
		}
		free(xattrnames);
	}
	
	folderinfo->num_files++;
	if ((fileinfo->st_flags & UF_COMPRESSED) == 0)
	{
		filesize_rounded = filesize = fileinfo->st_size;
		filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
		filesize += RFsize;
		filesize_rounded += RFsize;
		filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
		folderinfo->uncompressed_size += filesize;
		folderinfo->uncompressed_size_rounded += filesize_rounded;
		folderinfo->compressed_size += filesize;
		folderinfo->compressed_size_rounded += filesize_rounded;
		filesize = fileinfo->st_size;
		filesize += (filesize % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize % fileinfo->st_blksize) : 0;
		filesize += RFsize;
		filesize += (filesize % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize % fileinfo->st_blksize) : 0;
		filesize += compattrsize + xattrssize + (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
		folderinfo->total_size += filesize;
	}
	else
	{
		if (folderinfo->print_files)
		{
			if (folderinfo->print_info > 1)
			{
				printf("%s:\n", filepath);
				filesize = fileinfo->st_size;
				printf("File size (uncompressed data fork; reported size by Mac OS 10.6+ Finder): %s\n", getSizeStr(filesize, filesize));
				filesize_rounded = filesize = RFsize;
				filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
				printf("File size (compressed data fork - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n", getSizeStr(filesize, filesize_rounded));
				filesize_rounded = filesize = RFsize;
				filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
				filesize += compattrsize;
				filesize_rounded += compattrsize;
				printf("File size (compressed data fork): %s\n", getSizeStr(filesize, filesize_rounded));
				printf("Compression savings: %0.1f%%\n", (1.0 - (((double) RFsize + compattrsize) / fileinfo->st_size)) * 100.0);
				printf("Number of extended attributes: %d\n", numxattrs - numhiddenattr);
				printf("Total size of extended attribute data: %ld bytes\n", xattrssize);
				printf("Appoximate overhead of extended attributes: %ld bytes\n", ((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey));
				filesize = RFsize;
				filesize += (filesize % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize % fileinfo->st_blksize) : 0;
				filesize += compattrsize + xattrssize + (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
				printf("Appoximate total file size (compressed data fork + EA + EA overhead + file overhead): %s\n", getSizeStr(filesize, filesize));
			}
			else if (!folderinfo->compress_files)
			{
				printf("%s\n", filepath);
			}
		}

		filesize_rounded = filesize = fileinfo->st_size;
		filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
		folderinfo->uncompressed_size += filesize;
		folderinfo->uncompressed_size_rounded += filesize_rounded;
		filesize_rounded = filesize = RFsize;
		filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
		folderinfo->compressed_size += filesize;
		folderinfo->compressed_size_rounded += filesize_rounded;
		folderinfo->compattr_size += compattrsize;
		filesize = RFsize;
		filesize += (filesize % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize % fileinfo->st_blksize) : 0;
		filesize += compattrsize + xattrssize + (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
		folderinfo->total_size += filesize;
		folderinfo->num_compressed++;
	}
}

void process_folder(FTS *currfolder, struct folder_info *folderinfo)
{
	FTSENT *currfile;
	char *xattrnames, *curr_attr;
	ssize_t xattrnamesize, xattrssize, xattrsize;
	int numxattrs;
	bool volume_search;
	
	currfile = fts_read(currfolder);
	if (currfile == NULL)
	{
		fts_close(currfolder);
		return;
	}
	volume_search = (strncasecmp("/Volumes/", currfile->fts_path, 9) == 0 && strlen(currfile->fts_path) >= 8);
	
	do
	{
		if ((volume_search || strncasecmp("/Volumes/", currfile->fts_path, 9) != 0 || strlen(currfile->fts_path) < 9) &&
			(strncasecmp("/dev/", currfile->fts_path, 5) != 0 || strlen(currfile->fts_path) < 5))
		{
			if (S_ISDIR(currfile->fts_statp->st_mode) && currfile->fts_ino != 2)
			{
				if (currfile->fts_info & FTS_D)
				{
					if (!folderinfo->check_hard_links || !checkForHardLink(currfile->fts_path, currfile->fts_statp, folderinfo))
					{
						numxattrs = 0;
						xattrssize = 0;
						
						xattrnamesize = listxattr(currfile->fts_path, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
						
						if (xattrnamesize > 0)
						{
							xattrnames = (char *) malloc(xattrnamesize);
							if (xattrnames == NULL)
							{
								fprintf(stderr, "malloc error, unable to get folder information\n");
								continue;
							}
							if ((xattrnamesize = listxattr(currfile->fts_path, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
							{
								fprintf(stderr, "listxattr: %s\n", strerror(errno));
								free(xattrnames);
								continue;
							}
							for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
							{
								xattrsize = getxattr(currfile->fts_path, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
								if (xattrsize < 0)
								{
									fprintf(stderr, "getxattr: %s\n", strerror(errno));
									free(xattrnames);
									continue;
								}
								numxattrs++;
								xattrssize += xattrsize;
							}
							free(xattrnames);
						}
						folderinfo->num_folders++;
						folderinfo->total_size += xattrssize + (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFolder);
					}
					else
					{
						folderinfo->num_hard_link_folders++;
						fts_set(currfolder, currfile, FTS_SKIP);
						
						folderinfo->num_folders++;
						folderinfo->total_size += sizeof(HFSPlusCatalogFolder);
					}
				}
			}
			else if (S_ISREG(currfile->fts_statp->st_mode) || S_ISLNK(currfile->fts_statp->st_mode))
			{
				if (!folderinfo->check_hard_links || !checkForHardLink(currfile->fts_path, currfile->fts_statp, folderinfo))
				{
					if (folderinfo->compress_files && S_ISREG(currfile->fts_statp->st_mode))
					{
						compressFile(currfile->fts_path, currfile->fts_statp, folderinfo->maxSize, folderinfo->compressionlevel, folderinfo->minSavings, folderinfo->check_files);
						lstat(currfile->fts_path, currfile->fts_statp);
						if (((currfile->fts_statp->st_flags & UF_COMPRESSED) == 0) && folderinfo->print_files)
						{
							if (folderinfo->print_info > 0)
								printf("Unable to compress: ");
							printf("%s\n", currfile->fts_path);
						}
					}
					process_file(currfile->fts_path, currfile->fts_statp, folderinfo);
				}
				else
				{
					folderinfo->num_hard_link_files++;
					
					folderinfo->num_files++;
					folderinfo->total_size += sizeof(HFSPlusCatalogFile);
				}
			}
		}
		else
			fts_set(currfolder, currfile, FTS_SKIP);
	} while ((currfile = fts_read(currfolder)) != NULL);
	checkForHardLink(NULL, NULL, NULL);
	fts_close(currfolder);
}

void printUsage()
{
	printf("afsctool 1.2.3 (build 23)\n"
		   "Report if file is HFS+ compressed:                        afsctool [-v] file\n"
		   "Report if folder contains HFS+ compressed files:          afsctool [-fvv] folder\n"
		   "List HFS+ compressed files in folder:                     afsctool -l[fvv] folder\n"
		   "Decompress HFS+ compressed file or folder:                afsctool -d file/folder\n"
		   "Create archive file with compressed data in data fork:    afsctool -a[d] src dst\n"
		   "Extract HFS+ compression archive to file:                 afsctool -x[d] src dst\n"
		   "Apply HFS+ compression to file or folder:                 afsctool -c[klfvv] [compressionlevel [maxFileSize [minPercentSavings]]] file/folder\n\n"
		   "Options:\n"
		   "-v Increase verbosity level\n"
		   "-f Skip files if a hard link to them has already been processeed\n"
		   "-l List files that are HFS+ compressed (or if the -c option is given, files which fail to compress)\n"
		   "-k Verify file after compression, and revert file changes if file verification fails\n");
}

int main (int argc, const char * argv[])
{
	int i, j;
	struct stat fileinfo, dstfileinfo;
	struct folder_info folderinfo;
	FTS *currfolder;
	FTSENT *currfile;
	char *folderarray[2], *fullpath = NULL, *fullpathdst = NULL, *cwd;
	int printVerbose = 0, compressionlevel = 5;
	double minSavings = 25.0;
	long long int foldersize, foldersize_rounded, maxSize = 20971520;
	bool printDir = FALSE, decomp = FALSE, createfile = FALSE, extractfile = FALSE, applycomp = FALSE, fileCheck = FALSE, argIsFile, hardLinkCheck = FALSE, dstIsFile, free_src = FALSE, free_dst = FALSE;
	FILE *afscFile, *outFile;
	char *xattrnames, *curr_attr, header[4];
	ssize_t xattrnamesize, xattrsize, getxattrret, xattrPos;
	mode_t outFileMode;
	void *attr_buf;
	UInt16 big16;
	UInt64 big64;
	
	if (argc < 2)
	{
		printUsage();
		exit(EINVAL);
	}
	
	for (i = 1; i < argc && argv[i][0] == '-'; i++)
	{
		for (j = 1; j < strlen(argv[i]); j++)
		{
			switch (argv[i][j])
			{
				case 'l':
					if (createfile || extractfile || decomp)
					{
						printUsage();
						exit(EINVAL);
					}
					printDir = TRUE;
					break;
				case 'v':
					printVerbose++;
					break;
				case 'd':
					if (printDir || applycomp || hardLinkCheck)
					{
						printUsage();
						exit(EINVAL);
					}
					decomp = TRUE;
					break;
				case 'a':
					if (printDir || extractfile || applycomp || hardLinkCheck)
					{
						printUsage();
						exit(EINVAL);
					}
					createfile = TRUE;
					break;
				case 'x':
					if (printDir || createfile || applycomp || hardLinkCheck)
					{
						printUsage();
						exit(EINVAL);
					}
					extractfile = TRUE;
					break;
				case 'c':
					if (createfile || extractfile || decomp)
					{
						printUsage();
						exit(EINVAL);
					}
					applycomp = TRUE;
					break;
				case 'k':
					if (createfile || extractfile || decomp)
					{
						printUsage();
						exit(EINVAL);
					}
					fileCheck = TRUE;
					break;
				case 'f':
					if (createfile || extractfile || decomp)
					{
						printUsage();
						exit(EINVAL);
					}
					hardLinkCheck = TRUE;
					break;
				default:
					printUsage();
					exit(EINVAL);
					break;
			}
		}
	}
	
	if (applycomp && (argc - i > 1))
	{
		sscanf(argv[i], "%d", &compressionlevel);
		if (compressionlevel > 9 || compressionlevel < 1)
		{
			fprintf(stderr, "Invalid compression level; must be a number from 1 to 9\n");
			return -1;
		}
		i++;
	}
	
	if (applycomp && (argc - i > 1))
	{
		sscanf(argv[i], "%lld", &maxSize);
		i++;
	}
	
	if (applycomp && (argc - i > 1))
	{
		sscanf(argv[i], "%lf", &minSavings);
		if (minSavings > 99 || minSavings < 0)
		{
			fprintf(stderr, "Invalid minimum savings percentage; must be a number from 0 to 99\n");
			return -1;
		}
		i++;
	}
	
	if (i == argc || ((createfile || extractfile) && (argc - i < 2)))
	{
		printUsage();
		exit(EINVAL);
	}
	else if (createfile || extractfile)
	{
		if (argv[i+1][0] != '/')
		{
			cwd = getcwd(NULL, 0);
			if (cwd == NULL)
			{
				fprintf(stderr, "Unable to get PWD, exiting...\n");
				exit(EACCES);
			}
			free_dst = TRUE;
			fullpathdst = (char *) malloc(strlen(cwd) + strlen(argv[i+1]) + 2);
			sprintf(fullpathdst, "%s/%s", cwd, argv[i+1]);
			free(cwd);
		}
		else
			fullpathdst = (char *) argv[i+1];
	}
	
	if (argv[i][0] != '/')
	{
		cwd = getcwd(NULL, 0);
		if (cwd == NULL)
		{
			fprintf(stderr, "Unable to get PWD, exiting...\n");
			exit(EACCES);
		}
		free_src = TRUE;
		fullpath = (char *) malloc(strlen(cwd) + strlen(argv[i]) + 2);
		sprintf(fullpath, "%s/%s", cwd, argv[i]);
		free(cwd);
	}
	else
		fullpath = (char *) argv[i];
	
	if (lstat(fullpath, &fileinfo) < 0)
	{
		fprintf(stderr, "%s: %s\n", fullpath, strerror(errno));
		return -1;
	}
	
	argIsFile = ((fileinfo.st_mode & S_IFDIR) == 0);
	
	if (!argIsFile)
	{
		folderarray[0] = fullpath;
		folderarray[1] = NULL;
	}
	
	if ((createfile || extractfile) && lstat(fullpathdst, &dstfileinfo) >= 0)
	{
		dstIsFile = ((dstfileinfo.st_mode & S_IFDIR) == 0);
		fprintf(stderr, "%s: %s already exists at this path\n", fullpath, dstIsFile ? "File" : "Folder");
		return -1;
	}
	
	if (applycomp && argIsFile)
	{
		compressFile(fullpath, &fileinfo, maxSize, compressionlevel, minSavings, fileCheck);
		lstat(fullpath, &fileinfo);
	}
	
	if (createfile)
	{
		if (!argIsFile)
		{
			fprintf(stderr, "%s: File required, this is a folder\n", fullpath);
			return -1;
		}
		else if ((fileinfo.st_flags & UF_COMPRESSED) == 0)
		{
			fprintf(stderr, "%s: HFS+ compressed file required, this file is not HFS+ compressed\n", fullpath);
			return -1;
		}
		
		afscFile = fopen(fullpathdst, "w");
		if (afscFile == NULL)
		{
			fprintf(stderr, "%s: %s\n", fullpathdst, strerror(errno));
			return -1;
		}
		else
		{
			fprintf(afscFile, "afsc");
			big16 = EndianU16_NtoB(fileinfo.st_mode);
			if (fwrite(&big16, sizeof(mode_t), 1, afscFile) != 1)
			{
				fprintf(stderr, "%s: Error writing file\n", fullpathdst);
				return -1;
			}
			xattrnamesize = listxattr(fullpath, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
			
			if (xattrnamesize > 0)
			{
				xattrnames = (char *) malloc(xattrnamesize);
				if (xattrnames == NULL)
				{
					fprintf(stderr, "malloc error, unable to get file information\n");
					return -1;
				}
				if ((xattrnamesize = listxattr(fullpath, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
				{
					fprintf(stderr, "listxattr: %s\n", strerror(errno));
					free(xattrnames);
					return -1;
				}
				for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
				{
					xattrsize = getxattr(fullpath, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
					if (xattrsize < 0)
					{
						fprintf(stderr, "getxattr: %s\n", strerror(errno));
						free(xattrnames);
						return -1;
					}
					if (((strcmp(curr_attr, "com.apple.ResourceFork") == 0 && strlen(curr_attr) == 22) ||
						(strcmp(curr_attr, "com.apple.decmpfs") == 0 && strlen(curr_attr) == 17)) &&
						xattrsize != 0)
					{
						attr_buf = malloc(xattrsize);
						if (attr_buf == NULL)
						{
							fprintf(stderr, "malloc error, unable to get file information\n");
							return -1;
						}
						xattrPos = 0;
						do
						{
							getxattrret = getxattr(fullpath, curr_attr, attr_buf + xattrPos, xattrsize - xattrPos, xattrPos, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
							if (getxattrret < 0)
							{
								fprintf(stderr, "getxattr: %s\n", strerror(errno));
								free(xattrnames);
								return -1;
							}
							xattrPos += getxattrret;
						} while (xattrPos < xattrsize && getxattrret > 0);
						fprintf(afscFile, "%s", curr_attr);
						putc('\0', afscFile);
						big64 = EndianU64_NtoB(xattrsize);
						if (fwrite(&big64, sizeof(ssize_t), 1, afscFile) != 1 ||
							fwrite(attr_buf, xattrsize, 1, afscFile) != 1)
						{
							fprintf(stderr, "%s: Error writing file\n", fullpathdst);
							return -1;
						}
						free(attr_buf);
					}
				}
				free(xattrnames);
			}
			fclose(afscFile);
			if (decomp)
				decompressFile(fullpath, &fileinfo);
		}
	}
	else if (extractfile)
	{
		if (!argIsFile)
		{
			fprintf(stderr, "%s: File required, this is a folder\n", fullpath);
			return -1;
		}
		afscFile = fopen(fullpath, "r");
		if (afscFile == NULL)
		{
			fprintf(stderr, "%s: %s\n", fullpathdst, strerror(errno));
			return -1;
		}
		else
		{
			if (fread(header, 4, 1, afscFile) != 1)
			{
				fprintf(stderr, "%s: Error reading file\n", fullpath);
				return -1;
			}
			if (header[0] != 'a' ||
				header[1] != 'f' ||
				header[2] != 's' ||
				header[3] != 'c')
			{
				fprintf(stderr, "%s: Invalid header\n", fullpath);
				return -1;
			}
			outFile = fopen(fullpathdst, "w");
			if (outFile == NULL)
			{
				fprintf(stderr, "%s: %s\n", fullpathdst, strerror(errno));
				return -1;
			}
			else
			{
				fclose(outFile);
				if (fread(&outFileMode, sizeof(mode_t), 1, afscFile) != 1)
				{
					fprintf(stderr, "%s: Error reading file\n", fullpath);
					return -1;
				}
				outFileMode = EndianU16_BtoN(outFileMode);
				xattrnames = (char *) malloc(23);
				while (1)
				{
					for (j = 0; j < 23; j++)
					{
						xattrnames[j] = getc(afscFile);
						if (xattrnames[j] == '\0') break;
						if (xattrnames[j] == EOF) goto decomp_check;
					}
					if (j == 23 ||
						fread(&xattrsize, sizeof(ssize_t), 1, afscFile) != 1)
					{
						fprintf(stderr, "%s: Error reading file\n", fullpath);
						return -1;
					}
					xattrsize = EndianU64_BtoN(xattrsize);
					attr_buf = malloc(xattrsize);
					if (attr_buf == NULL)
					{
						fprintf(stderr, "malloc error, unable to set file information\n");
						return -1;
					}
					if (fread(attr_buf, xattrsize, 1, afscFile) != 1)
					{
						fprintf(stderr, "%s: Error reading file\n", fullpath);
						return -1;
					}
					if (setxattr(fullpathdst, xattrnames, attr_buf, xattrsize, 0, XATTR_NOFOLLOW | XATTR_CREATE) < 0)
					{
						fprintf(stderr, "setxattr: %s\n", strerror(errno));
						return -1;
					}
					free(attr_buf);
				}
				fprintf(stderr, "%s: Error reading file\n", fullpath);
				return -1;
			decomp_check:
				if (chflags(fullpathdst, UF_COMPRESSED) < 0)
				{
					fprintf(stderr, "chflags: %s\n", strerror(errno));
					return -1;
				}
				if (chmod(fullpathdst, outFileMode) < 0)
				{
					fprintf(stderr, "chmod: %s\n", strerror(errno));
					return -1;
				}
				if (decomp)
				{
					if (lstat(fullpathdst, &dstfileinfo) < 0)
					{
						fprintf(stderr, "%s: %s\n", fullpathdst, strerror(errno));
						return -1;
					}
					decompressFile(fullpathdst, &dstfileinfo);
				}
			}
		}
	}
	else if (decomp && argIsFile)
	{
		decompressFile(fullpath, &fileinfo);
	}
	else if (decomp)
	{
		if ((currfolder = fts_open(folderarray, FTS_PHYSICAL, NULL)) == NULL)
		{
			fprintf(stderr, "%s: %s\n", fullpath, strerror(errno));
			exit(EACCES);
		}
		while ((currfile = fts_read(currfolder)) != NULL)
			if ((currfile->fts_statp->st_mode & S_IFDIR) == 0)
				decompressFile(currfile->fts_path, currfile->fts_statp);
		fts_close(currfolder);
	}
	else if (argIsFile && printVerbose == 0)
	{
		if (applycomp)
		{
			if ((fileinfo.st_flags & UF_COMPRESSED) == 0)
				printf("Unable to compress file.\n");
		}
		else
		{
			if ((fileinfo.st_flags & UF_COMPRESSED) != 0)
				printf("File is HFS+ compressed.\n");
			else
				printf("File is not HFS+ compressed.\n");
		}
	}
	else if (argIsFile && printVerbose > 0)
	{
		printFileInfo(fullpath, &fileinfo, applycomp);
	}
	else if (!argIsFile)
	{
		if ((currfolder = fts_open(folderarray, FTS_PHYSICAL, NULL)) == NULL)
		{
			fprintf(stderr, "%s: %s\n", fullpath, strerror(errno));
			exit(EACCES);
		}
		folderinfo.uncompressed_size = 0;
		folderinfo.uncompressed_size_rounded = 0;
		folderinfo.compressed_size = 0;
		folderinfo.compressed_size_rounded = 0;
		folderinfo.compattr_size = 0;
		folderinfo.total_size = 0;
		folderinfo.num_compressed = 0;
		folderinfo.num_files = 0;
		folderinfo.num_hard_link_files = 0;
		folderinfo.num_folders = 0;
		folderinfo.num_hard_link_folders = 0;
		folderinfo.print_info = printVerbose;
		folderinfo.print_files = printDir;
		folderinfo.compress_files = applycomp;
		folderinfo.check_files = fileCheck;
		folderinfo.compressionlevel = compressionlevel;
		folderinfo.minSavings = minSavings;
		folderinfo.maxSize = maxSize;
		folderinfo.check_hard_links = hardLinkCheck;
		process_folder(currfolder, &folderinfo);
		folderinfo.num_folders--;
		if (printVerbose > 0 || !printDir)
		{
			if (printDir) printf("\n");
			printf("%s:\n", fullpath);
			if (folderinfo.num_compressed == 0 && !applycomp)
				printf("Folder contains no compressed files\n");
			else if (folderinfo.num_compressed == 0 && applycomp)
				printf("No compressable files in folder\n");
			else
				printf("Number of HFS+ compressed files: %lld\n", folderinfo.num_compressed);
			if (printVerbose > 0)
			{
				printf("Total number of files: %lld\n", folderinfo.num_files);
				if (hardLinkCheck)
					printf("Total number of file hard links: %lld\n", folderinfo.num_hard_link_files);
				printf("Total number of folders: %lld\n", folderinfo.num_folders);
				if (hardLinkCheck)
					printf("Total number of folder hard links: %lld\n", folderinfo.num_hard_link_folders);
				printf("Total number of items (number of files + number of folders): %lld\n", folderinfo.num_files + folderinfo.num_folders);
				foldersize = folderinfo.uncompressed_size;
				foldersize_rounded = folderinfo.uncompressed_size_rounded;
				if ((folderinfo.num_hard_link_files == 0 && folderinfo.num_hard_link_folders == 0) || !hardLinkCheck)
					printf("Folder size (uncompressed; reported size by Mac OS 10.6+ Finder): %s\n", getSizeStr(foldersize, foldersize_rounded));
				else
					printf("Folder size (uncompressed): %s\n", getSizeStr(foldersize, foldersize_rounded));
				foldersize = folderinfo.compressed_size;
				foldersize_rounded = folderinfo.compressed_size_rounded;
				if ((folderinfo.num_hard_link_files == 0 && folderinfo.num_hard_link_folders == 0) || !hardLinkCheck)
					printf("Folder size (compressed - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n", getSizeStr(foldersize, foldersize_rounded));
				else
					printf("Folder size (compressed - decmpfs xattr): %s\n", getSizeStr(foldersize, foldersize_rounded));
				foldersize = folderinfo.compressed_size + folderinfo.compattr_size;
				foldersize_rounded = folderinfo.compressed_size_rounded + folderinfo.compattr_size;
				printf("Folder size (compressed): %s\n", getSizeStr(foldersize, foldersize_rounded));
				printf("Compression savings: %0.1f%%\n", (1.0 - ((float) (folderinfo.compressed_size + folderinfo.compattr_size) / folderinfo.uncompressed_size)) * 100.0);
				foldersize = folderinfo.total_size;
				printf("Appoximate total folder size (files + file overhead + folder overhead): %s\n", getSizeStr(foldersize, foldersize));
			}
		}
	}
	
	if (free_src)
		free(fullpath);
	if (free_dst)
		free(fullpathdst);
	
	return 0;
}
