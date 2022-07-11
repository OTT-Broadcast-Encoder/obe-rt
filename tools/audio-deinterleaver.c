#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

static void _usage(const char *program)
{
	fprintf(stderr, "%s -i <audio.raw> -c <8|16> [-x]\n", program);
	fprintf(stderr, " -i <audio.raw>\n");
	fprintf(stderr, " -c # channels in file, 8 or 16. [def: 16]\n");
	fprintf(stderr, " -x Show all audio as hex. [def: disabled]\n");
}

int main(int argc, char *argv[])
{
	int nr = 0;
	int channel_depth = 32;
	unsigned char buf[8];
	int opt;

	FILE *ifh = NULL;
	const char *filename = NULL;
	int max_channels = 16; /* DUO 2*/
	int showHex = 0;

	while ((opt = getopt(argc, argv, "hc:i:x")) != -1) {
		switch (opt) {
		case 'i':
			filename = optarg;
			break;
		case 'c':
			max_channels = atoi(optarg);
			if ((max_channels != 8) && (max_channels != 16)) {
				_usage(argv[0]);
				fprintf(stderr, "-c has an invalid argument, aborting.\n");
				return -1;
			}
			break;
		case 'x':
			showHex = 1;
			break;
		default:
			_usage(argv[0]);
			return -1;
		}
	}

	if (!filename) {
		_usage(argv[0]);
		return -1;
	} 

	ifh = fopen(filename, "rb");
	if (!ifh) {
		fprintf(stderr, "Unable to open raw audio file for read\n");
		return -1;
	}

	FILE *ofh[16] = { 0 };

	for (int i = 0; i < max_channels / 1; i++) {
		char fn[64];
		sprintf(fn, "audio-channel%02d-s32.raw", i);
		ofh[i] = fopen(fn, "wb");
		if (!ofh[i]) {
			fprintf(stderr, "Unable to open audio file %s for write\n", fn);
			return -1;
		}
	}

	while (!feof(ifh)) {
		if (showHex)
			printf("\n%08d: ", nr++);
		for (int i = 0; i < max_channels / 1; i++) {
			memset(buf, 0, sizeof(buf));

			fread(&buf[0], 1, 1 * (channel_depth / 8), ifh);
			if (feof(ifh))
				break;

			if (showHex) {
				for (int z = 0; z < 1 * (channel_depth / 8); z++) {
					if (buf[z])
						printf("%02x ", buf[z]);
					else
						printf("   ");
				}
				printf("  :  ");
			}

			fwrite(&buf[0], 1, 1 * (channel_depth / 8), ofh[i]);
		}
	}

	fclose(ifh);

	for (int i = 0; i < max_channels; i++) {
		if (ofh[i])
			fclose(ofh[i]);
	}

	return 0;
}
